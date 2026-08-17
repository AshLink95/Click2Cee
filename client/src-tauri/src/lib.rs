const FRAG: usize = 1400;
const HDR: usize = 8; // seq u32, index u16, count u16
const PKT: usize = HDR + FRAG;

pub struct Receiver {
    sock: std::net::UdpSocket,
    pkt: [u8; PKT], // one datagram
    frame: Vec<u8>, // reassembly, keeps its allocation between frames
    next: u16,      // fragment index expected next, u16::MAX once torn
}

// Mutex because recv_frame mutates, OnceLock because it is built once in run().
static RX: std::sync::OnceLock<std::sync::Mutex<Receiver>> = std::sync::OnceLock::new();

impl Receiver {
    pub fn bind(addr: &str, port: u16) -> std::io::Result<Self> {
        Ok(Self {
            sock: std::net::UdpSocket::bind((addr, port))?,
            pkt: [0u8; PKT],
            frame: Vec::new(),
            next: 0,
        })
    }

    /// Ok(None) means the fragment was absorbed and the frame is not whole yet.
    /// A lost or reordered fragment drops the whole frame
    pub fn recv_frame(&mut self) -> std::io::Result<Option<&[u8]>> {
        let n = self.sock.recv(&mut self.pkt)?;
        if n < HDR { return Ok(None); }
        // from_be_bytes is the other half of the server's htonl/htons
        let idx = u16::from_be_bytes(self.pkt[4..6].try_into().unwrap());
        let count = u16::from_be_bytes(self.pkt[6..8].try_into().unwrap());

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
        self.next += 1;

        if self.next == count {
            return Ok(Some(&self.frame));
        }
        Ok(None)
    }
}

/// Binds once, before the app starts, so a busy port fails here instead of as a
/// mystery error on the first frame.
fn init_recv(addr: &str, port: u16) -> std::io::Result<()> {
    let rx = Receiver::bind(addr, port)?;
    RX.set(std::sync::Mutex::new(rx))
        .map_err(|_| std::io::Error::new(std::io::ErrorKind::AlreadyExists, "already bound"))
}

/// Blocks until a whole frame is assembled, then hands it to the webview.
#[tauri::command]
fn rcv_udp() -> Result<Vec<u8>, String> {
    let mut rx = RX
        .get()
        .ok_or("receiver not initialised")?
        .lock()
        .map_err(|_| "receiver poisoned")?;

    loop {
        // is_some() drops the borrow on rx immediately, so the clone below can
        // take its own
        if rx.recv_frame().map_err(|e| e.to_string())?.is_some() {
            return Ok(rx.frame.clone());
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // any interface, so a server on another machine works too, not just loopback
    init_recv("0.0.0.0", 5000).expect("failed to bind udp receiver");

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![rcv_udp])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
