const FRAG: usize = 1400;
const HDR: usize = 8; // seq u32, index u16, count u16
const PKT: usize = HDR + FRAG;
const MAX_FRAME: usize = 4 << 20; // ceiling on reassembly, past any real frame

const MAX_KEYS: usize = 64;
const MAX_NAME: usize = 16; // "ControlRight" is the longest we send
const IN_PKT: usize = 6 + MAX_KEYS * (1 + MAX_NAME); // count, btn, x, y, then the keys

pub struct Receiver {
    sock: std::net::UdpSocket,
    pkt: [u8; PKT], // one datagram
    frame: Vec<u8>, // reassembly, keeps its allocation between frames
    next: u16,      // fragment index expected next, u16::MAX once torn
}

pub struct Sender {
    sock: std::net::UdpSocket,
    pkt: [u8; IN_PKT]
}

// Mutex because invoked fns mutate, OnceLock because built once in run()
static RX: std::sync::OnceLock<std::sync::Mutex<Receiver>> = std::sync::OnceLock::new();
static SX: std::sync::OnceLock<std::sync::Mutex<Sender>> = std::sync::OnceLock::new();

impl Receiver {
    pub fn connect(saddr: &str, rport: u16, caddr: &str, pport: u16) -> std::io::Result<Self> {
        let sock = std::net::UdpSocket::bind((caddr, pport))?;
        sock.connect((saddr, rport))?; // pins the peer so recv() needs no address
        Ok(Self { sock, pkt: [0u8; PKT], frame: Vec::new(), next: 0 })
    }

    /// Ok(None) means the fragment was absorbed and the frame is not whole yet.
    /// A lost or reordered fragment drops the whole frame
    pub fn recv_frame(&mut self) -> std::io::Result<Option<&[u8]>> {
        let n = match self.sock.recv(&mut self.pkt) {
            Ok(n) => n,
            // An icmp bounce or an interrupted syscall is one bad moment, not a
            // finished socket, and clears itself by the next call. Losing a
            // frame over it is the right price. Every other kind keeps failing,
            // so it is left to propagate rather than spin here forever.
            Err(e) if matches!(
                e.kind(),
                std::io::ErrorKind::Interrupted
                    | std::io::ErrorKind::WouldBlock
                    | std::io::ErrorKind::ConnectionReset
            ) => {
                self.next = u16::MAX; // half-built frame is no longer trustworthy
                return Ok(None);
            }
            Err(e) => return Err(e),
        };
        if n < HDR { return Ok(None); }
        // from_be_bytes is the other half of the server's htonl/htons
        let idx = u16::from_be_bytes(self.pkt[4..6].try_into().unwrap());
        let count = u16::from_be_bytes(self.pkt[6..8].try_into().unwrap());
        // a count of 0 is never reached by next, and an index past the end is
        // nonsense: either one leaves the buffer growing with no way to finish
        if count == 0 || idx >= count { return Ok(None); }

        if idx == 0 {
            self.frame.clear();
            self.next = 0;
        } else if idx != self.next {
            self.next = u16::MAX; // torn, wait for the next frame to start
        }
        if self.next == u16::MAX {
            return Ok(None);
        }

        self.frame.extend_from_slice(&self.pkt[HDR..n]);
        if self.frame.len() > MAX_FRAME {
            self.frame.clear();
            self.next = u16::MAX;
            return Ok(None);
        }
        self.next += 1;

        if self.next == count {
            return Ok(Some(&self.frame));
        }
        Ok(None)
    }
}

impl Sender {
    pub fn connect(addr: &str, port: u16) -> std::io::Result<Self> {
        let sock = std::net::UdpSocket::bind("0.0.0.0:0")?; // any free local port
        sock.connect((addr, port))?; // pins the peer so send() needs no address
        Ok(Self { sock, pkt: [0; IN_PKT] })
    }

    pub fn send_input(&mut self, keys: &[String], x: u16, y: u16, btn: u8) -> std::io::Result<()> {
        let n = keys.len().min(MAX_KEYS);
        self.pkt[0] = n as u8;
        self.pkt[1] = btn;
        self.pkt[2..4].copy_from_slice(&x.to_be_bytes());
        self.pkt[4..6].copy_from_slice(&y.to_be_bytes());

        let mut off = 6;
        for k in keys.iter().take(n) {
            let b = k.as_bytes();
            let len = b.len().min(MAX_NAME);
            self.pkt[off] = len as u8;
            self.pkt[off + 1..off + 1 + len].copy_from_slice(&b[..len]);
            off += 1 + len;
        }
        self.sock.send(&self.pkt[..off])?;
        Ok(())
    }
}

fn init_recv(saddr: &str, rport: u16, caddr: &str, pport: u16) -> std::io::Result<()> {
    let rx = Receiver::connect(saddr, rport, caddr, pport)?;
    RX.set(std::sync::Mutex::new(rx))
        .map_err(|_| std::io::Error::new(std::io::ErrorKind::AlreadyExists, "already connected"))
}

fn init_send(addr: &str, port: u16) -> std::io::Result<()> {
    let sx = Sender::connect(addr, port)?;
    SX.set(std::sync::Mutex::new(sx))
        .map_err(|_| std::io::Error::new(std::io::ErrorKind::AlreadyExists, "already connected"))
}

/// Blocks until a whole frame is assembled, then hands it to the webview.
/// async so tauri runs it off the main thread, otherwise the blocking recv
/// freezes the window.
#[tauri::command]
async fn rcv_udp() -> Result<Vec<u8>, String> {
    let mut rx = RX
        .get()
        .ok_or("receiver not initialised")?
        .lock()
        .map_err(|_| "receiver poisoned")?;

    loop {
        if rx.recv_frame().map_err(|e| e.to_string())?.is_some() {
            return Ok(rx.frame.clone());
        }
    }
}

#[tauri::command]
fn snd_input(keys: Vec<String>, x: u16, y: u16, btn: u8) -> Result<(), String> {
    // println!("keys: {:?}, x: {}, y: {}, btn: {}", keys, x, y, btn); //dbg
    SX.get()
        .ok_or("sender not initialised")?
        .lock()
        .map_err(|_| "sender poisoned")?
        .send_input(&keys, x, y, btn)
        .map_err(|e| e.to_string())
}

fn env(key: &str, fallback: &str) -> String {
    let mut path = std::path::PathBuf::from(".env");
    for _ in 0..4 {
        if let Ok(text) = std::fs::read_to_string(&path) {
            for line in text.lines() {
                let line = line.trim();
                if line.is_empty() || line.starts_with('#') {
                    continue;
                }
                if let Some((k, v)) = line.split_once('=') {
                    if k.trim() == key {
                        return v.trim().to_string();
                    }
                }
            }
            break;
        }
        path = std::path::Path::new("..").join(path);
    }
    fallback.to_string()
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let host: String = env("SERVER_HOST", "127.0.0.1");
    let clnt: String = env("CLIENT_HOST", "127.0.0.1");
    let port_recv: u16 = env("VIDEO_PORT", "5000").parse().unwrap_or(5000);
    let port_play: u16 = env("PLAYER_PORT", "5002").parse().unwrap_or(5002);
    let port_send: u16 = env("INPUT_PORT", "5001").parse().unwrap_or(5001);
    init_recv(&host, port_recv, &clnt, port_play)
        .expect("failed to connect udp receiver");
    init_send(&host, port_send)
        .expect("failed to connect input sender");

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![rcv_udp, snd_input])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
