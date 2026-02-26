#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#define SAMPLE_RATE     48000
#define CHANNELS        2
#define BYTES_PER_SAMP  2
#define FRAME_BYTES     (CHANNELS * BYTES_PER_SAMP)

#define PORT            4010
#define MAX_UDP_PAYLOAD 2048
#define RING_FRAMES     (48000)
#define HEADER_BYTES    20
#define PROTO_MAGIC     0x5A4B  /* "ZK" little-endian */
#define PROTO_VERSION   1

/* Default start fill — overridable via argv[2] */
#define DEFAULT_START_FILL_MS   15

static uint8_t ring[RING_FRAMES * FRAME_BYTES];
static volatile uint32_t wpos_frames = 0;
static volatile uint32_t rpos_frames = 0;
static volatile int running = 1;

static inline uint32_t rb_used_frames(void) {
  uint32_t w = __atomic_load_n(&wpos_frames, __ATOMIC_ACQUIRE);
  uint32_t r = __atomic_load_n(&rpos_frames, __ATOMIC_ACQUIRE);
  return (w >= r) ? (w - r) : (RING_FRAMES - (r - w));
}
static inline uint32_t rb_free_frames(void) {
  return (RING_FRAMES - 1) - rb_used_frames();
}

static void rb_write_frames(const uint8_t *src, uint32_t frames) {
  if (frames > rb_free_frames()) return;
  uint32_t w = wpos_frames;
  uint32_t bytes = frames * FRAME_BYTES;
  uint32_t w_bytes = w * FRAME_BYTES;
  uint32_t tail = (RING_FRAMES * FRAME_BYTES) - w_bytes;

  if (bytes <= tail) memcpy(&ring[w_bytes], src, bytes);
  else {
    memcpy(&ring[w_bytes], src, tail);
    memcpy(&ring[0], src + tail, bytes - tail);
  }
  __atomic_store_n(&wpos_frames, (w + frames) % RING_FRAMES, __ATOMIC_RELEASE);
}

static uint32_t rb_read_frames(uint8_t *dst, uint32_t frames) {
  uint32_t avail = rb_used_frames();
  if (frames > avail) frames = avail;

  uint32_t r = rpos_frames;
  uint32_t bytes = frames * FRAME_BYTES;
  uint32_t r_bytes = r * FRAME_BYTES;
  uint32_t tail = (RING_FRAMES * FRAME_BYTES) - r_bytes;

  if (bytes <= tail) memcpy(dst, &ring[r_bytes], bytes);
  else {
    memcpy(dst, &ring[r_bytes], tail);
    memcpy(dst + tail, &ring[0], bytes - tail);
  }
  __atomic_store_n(&rpos_frames, (r + frames) % RING_FRAMES, __ATOMIC_RELEASE);
  return frames;
}

/* --- Config & status --- */
#define CONFIG_PATH       "/etc/zinkos/config"
#define DEFAULT_STATUS_PATH "/run/zinkos/status"
#define DEFAULT_IDLE_TIMEOUT 5

static struct {
  int idle_timeout_s;
  char status_path[256];
} cfg = {
  .idle_timeout_s = DEFAULT_IDLE_TIMEOUT,
  .status_path    = DEFAULT_STATUS_PATH,
};

static void parse_config(void) {
  FILE *f = fopen(CONFIG_PATH, "r");
  if (!f) return;  /* missing config is fine — use defaults */

  char line[512];
  while (fgets(line, sizeof(line), f)) {
    /* strip newline */
    line[strcspn(line, "\n")] = '\0';

    /* skip comments and blank lines */
    if (line[0] == '#' || line[0] == '\0') continue;

    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    const char *key = line;
    const char *val = eq + 1;

    if (strcmp(key, "idle_timeout_s") == 0) {
      int v = atoi(val);
      if (v > 0) cfg.idle_timeout_s = v;
    } else if (strcmp(key, "status_path") == 0) {
      if (val[0] == '/')
        snprintf(cfg.status_path, sizeof(cfg.status_path), "%s", val);
    }
    /* unknown keys silently ignored */
  }
  fclose(f);
}

static void write_status(const char *status) {
  char tmp[280];
  snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.status_path);

  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;

  size_t len = strlen(status);
  write(fd, status, len);
  write(fd, "\n", 1);
  close(fd);
  rename(tmp, cfg.status_path);
}

static void ensure_status_dir(void) {
  /* extract directory from status_path */
  char dir[256];
  snprintf(dir, sizeof(dir), "%s", cfg.status_path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
    mkdir(dir, 0755);  /* ignore error — may already exist */
  }
}

static void set_thread_realtime(int priority) {
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = priority;
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

// UDP receive thread — runs independently of ALSA playback
static void *recv_thread(void *arg) {
  int fd = *(int *)arg;
  uint8_t pkt[MAX_UDP_PAYLOAD];

  set_thread_realtime(70);

  while (running) {
    ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
    if (n <= 0) continue;
    if (n <= HEADER_BYTES) continue;

    /* Validate magic and version */
    uint16_t magic = (uint16_t)pkt[0] | ((uint16_t)pkt[1] << 8);
    if (magic != PROTO_MAGIC) {
      static uint64_t bad_magic = 0;
      if (++bad_magic == 1 || bad_magic % 1000 == 0)
        fprintf(stderr, "dropping packets with bad magic (old sender?) — count: %lu\n", (unsigned long)bad_magic);
      continue;
    }
    if (pkt[2] > PROTO_VERSION) {
      static uint64_t bad_ver = 0;
      if (++bad_ver == 1 || bad_ver % 1000 == 0)
        fprintf(stderr, "sender protocol v%u > receiver v%u — update the receiver\n", pkt[2], PROTO_VERSION);
      continue;
    }

    uint8_t *pcm_data = pkt + HEADER_BYTES;
    ssize_t pcm_len = n - HEADER_BYTES;
    if ((pcm_len % FRAME_BYTES) != 0) continue;
    rb_write_frames(pcm_data, (uint32_t)(pcm_len / FRAME_BYTES));
  }
  return NULL;
}

int main(int argc, char **argv) {
  const char *alsa_dev = (argc > 1) ? argv[1] : "hw:0,0";
  uint32_t start_fill_ms = DEFAULT_START_FILL_MS;
  uint32_t period_frames = 240;
  uint16_t port = PORT;
  if (argc > 2) {
    int val = atoi(argv[2]);
    if (val > 0 && val <= 500) start_fill_ms = (uint32_t)val;
    else fprintf(stderr, "Ignoring invalid start-fill %s ms (valid: 1–500)\n", argv[2]);
  }
  if (argc > 3) {
    int val = atoi(argv[3]);
    if (val >= 48 && val <= 4800) period_frames = (uint32_t)val;
    else fprintf(stderr, "Ignoring invalid period %s frames (valid: 48–4800)\n", argv[3]);
  }
  if (argc > 4) {
    int val = atoi(argv[4]);
    if (val > 0 && val <= 65535) port = (uint16_t)val;
    else fprintf(stderr, "Ignoring invalid port %s (valid: 1–65535)\n", argv[4]);
  }

  parse_config();
  ensure_status_dir();
  write_status("idle");
  fprintf(stderr, "Config: idle_timeout_s=%d status_path=%s\n",
          cfg.idle_timeout_s, cfg.status_path);

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { perror("socket"); return 1; }

  int rcvbuf = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }

  snd_pcm_t *pcm = NULL;
  if (snd_pcm_open(&pcm, alsa_dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
    fprintf(stderr, "snd_pcm_open failed for %s\n", alsa_dev);
    return 1;
  }

  // --- HW params ---
  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(pcm, hw);
  snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);

  unsigned int rate = SAMPLE_RATE;
  snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);

  snd_pcm_uframes_t period = period_frames;
  snd_pcm_uframes_t buffer = period * 3;

  snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
  snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

  if (snd_pcm_hw_params(pcm, hw) < 0) {
    fprintf(stderr, "snd_pcm_hw_params failed\n");
    return 1;
  }

  snd_pcm_uframes_t actual_period, actual_buffer;
  snd_pcm_hw_params_get_period_size(hw, &actual_period, NULL);
  snd_pcm_hw_params_get_buffer_size(hw, &actual_buffer);
  fprintf(stderr, "ALSA: rate=%u period=%lu buffer=%lu\n",
          rate, actual_period, actual_buffer);

  // --- SW params: don't auto-start, we start manually after pre-fill ---
  snd_pcm_sw_params_t *sw;
  snd_pcm_sw_params_alloca(&sw);
  snd_pcm_sw_params_current(pcm, sw);
  snd_pcm_sw_params_set_start_threshold(pcm, sw, actual_period);
  snd_pcm_sw_params_set_avail_min(pcm, sw, actual_period);
  if (snd_pcm_sw_params(pcm, sw) < 0) {
    fprintf(stderr, "snd_pcm_sw_params failed\n");
    return 1;
  }

  snd_pcm_prepare(pcm);
  set_thread_realtime(60);

  // Start receive thread
  pthread_t rthr;
  pthread_create(&rthr, NULL, recv_thread, &fd);

  uint8_t out[1024 * FRAME_BYTES];

  const uint32_t start_fill_frames = (start_fill_ms * SAMPLE_RATE) / 1000;
  fprintf(stderr, "Zinkos RX UDP :%u @ %uHz, ALSA %s, start fill ~%ums (%u frames)\n",
          port, rate, alsa_dev, start_fill_ms, start_fill_frames);

  // Wait for ring buffer to accumulate enough
  while (rb_used_frames() < start_fill_frames) {
    usleep(1000);
  }
  fprintf(stderr, "Ring buffer ready: %u frames\n", rb_used_frames());

  // Pre-fill ALSA buffer (but don't drain ring buffer dry)
  snd_pcm_uframes_t prefilled = 0;
  while (prefilled + actual_period <= actual_buffer) {
    uint32_t avail = rb_used_frames();
    if (avail < actual_period) break;  // keep cushion, don't drain ring buffer
    uint32_t got = rb_read_frames(out, (uint32_t)actual_period);
    if (got < actual_period) break;

    snd_pcm_sframes_t w = snd_pcm_writei(pcm, out, actual_period);
    if (w > 0) prefilled += w;
    else break;
  }
  fprintf(stderr, "ALSA pre-filled: %lu frames, ring buffer remaining: %u frames\n",
          prefilled, rb_used_frames());

  // Start playback
  snd_pcm_start(pcm);
  write_status("playing");

  int is_playing = 1;
  struct timespec last_data_ts;
  clock_gettime(CLOCK_MONOTONIC, &last_data_ts);

  while (running) {
    uint32_t got = rb_read_frames(out, (uint32_t)actual_period);
    if (got < actual_period)
      memset(out + got * FRAME_BYTES, 0, (actual_period - got) * FRAME_BYTES);

    /* Check if audio is non-silent */
    int has_audio = 0;
    if (got > 0) {
      const int16_t *samples = (const int16_t *)out;
      for (uint32_t i = 0; i < got * CHANNELS; i++) {
        if (samples[i] != 0) { has_audio = 1; break; }
      }
    }

    /* Track idle/playing transitions using wall clock */
    if (has_audio) {
      clock_gettime(CLOCK_MONOTONIC, &last_data_ts);
      if (!is_playing) { write_status("playing"); is_playing = 1; }
    } else if (is_playing) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long elapsed_s = now.tv_sec - last_data_ts.tv_sec;
      if (elapsed_s >= cfg.idle_timeout_s) {
        write_status("idle");
        is_playing = 0;
      }
    }

    snd_pcm_sframes_t w = snd_pcm_writei(pcm, out, actual_period);
    if (w < 0) {
      fprintf(stderr, "ALSA xrun, recovering\n");
      w = snd_pcm_recover(pcm, w, 1);
      if (w < 0) {
        fprintf(stderr, "ALSA write failed: %s\n", snd_strerror(w));
        break;
      }
    }
  }

  running = 0;
  write_status("idle");
  pthread_join(rthr, NULL);
  snd_pcm_close(pcm);
  close(fd);
  return 0;
}
