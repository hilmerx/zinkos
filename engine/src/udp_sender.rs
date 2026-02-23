use std::io;
use std::net::{SocketAddr, ToSocketAddrs, UdpSocket};

use crate::config::{DSCP_EF, UDP_SNDBUF_SIZE};

/// UDP socket wrapper with QoS. Pacing is handled by the caller.
pub struct UdpSender {
    socket: UdpSocket,
    target: SocketAddr,
}

impl UdpSender {
    pub fn new(target_ip: &str, target_port: u16) -> io::Result<Self> {
        let socket = UdpSocket::bind("0.0.0.0:0")?;
        // Use ToSocketAddrs for DNS/mDNS resolution — resolves both raw IPs
        // (e.g. "192.168.1.87") and hostnames (e.g. "1337.local")
        let target = format!("{target_ip}:{target_port}")
            .to_socket_addrs()?
            .find(|a| a.is_ipv4())
            .ok_or_else(|| io::Error::new(io::ErrorKind::AddrNotAvailable,
                format!("cannot resolve {target_ip}")
            ))?;

        // Set send buffer size
        Self::set_sndbuf(&socket, UDP_SNDBUF_SIZE)?;

        // Set DSCP for low-latency traffic (best-effort on most home routers, but helps on managed networks)
        Self::set_dscp(&socket);

        // Non-blocking not needed — network thread can block briefly
        socket.set_nonblocking(false)?;

        Ok(Self {
            socket,
            target,
        })
    }

    fn set_sndbuf(socket: &UdpSocket, size: u32) -> io::Result<()> {
        use std::os::fd::AsRawFd;
        let fd = socket.as_raw_fd();
        let size = size as libc::c_int;
        let ret = unsafe {
            libc::setsockopt(
                fd,
                libc::SOL_SOCKET,
                libc::SO_SNDBUF,
                &size as *const _ as *const libc::c_void,
                std::mem::size_of::<libc::c_int>() as libc::socklen_t,
            )
        };
        if ret < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    fn set_dscp(socket: &UdpSocket) {
        use std::os::fd::AsRawFd;
        let fd = socket.as_raw_fd();
        let tos = DSCP_EF as libc::c_int;
        unsafe {
            libc::setsockopt(
                fd,
                libc::IPPROTO_IP,
                libc::IP_TOS,
                &tos as *const _ as *const libc::c_void,
                std::mem::size_of::<libc::c_int>() as libc::socklen_t,
            );
        }
        // Ignore error — DSCP is optional (may fail without root)
    }

    /// Send a packet immediately.
    pub fn send(&self, data: &[u8]) -> io::Result<usize> {
        self.socket.send_to(data, self.target)
    }

    pub fn target(&self) -> SocketAddr {
        self.target
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::UdpSocket;
    use std::time::Duration;

    #[test]
    fn send_and_receive_on_localhost() {
        let receiver = UdpSocket::bind("127.0.0.1:0").unwrap();
        let recv_addr = receiver.local_addr().unwrap();
        receiver
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();

        let sender = UdpSender::new("127.0.0.1", recv_addr.port()).unwrap();

        let payload = b"hello zinkos";
        sender.send(payload).unwrap();

        let mut buf = [0u8; 1024];
        let (n, _from) = receiver.recv_from(&mut buf).unwrap();
        assert_eq!(&buf[..n], payload);
    }

    #[test]
    fn full_size_packet() {
        let receiver = UdpSocket::bind("127.0.0.1:0").unwrap();
        let recv_addr = receiver.local_addr().unwrap();
        receiver
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();

        let sender = UdpSender::new("127.0.0.1", recv_addr.port()).unwrap();

        // Send a 900-byte packet (our max)
        let pkt = vec![0xABu8; 900];
        sender.send(&pkt).unwrap();

        let mut buf = [0u8; 2048];
        let (n, _) = receiver.recv_from(&mut buf).unwrap();
        assert_eq!(n, 900);
    }
}
