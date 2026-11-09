//! bo2-logd — receives the Bo2-APK app's logs over Tailscale and stores them per session.
//!
//! Protocol (HTTP/1.1, plain text; see README.md):
//!   GET  /v1/ping    -> 200 "bo2-logd ok\n"
//!   POST /v1/ingest  X-Bo2-Session: <token>   1-96 characters of A-Z a-z 0-9 . _ -
//!                    X-Bo2-Kind: hello|log|prof
//!                    X-Bo2-Seq: <u64>          optional: a batch whose seq is already stored for that session and
//!                                              kind (a retry) is acknowledged and dropped
//!                    body: UTF-8 text lines, at most 4 MB (413 beyond)          -> 204
//! Storage (--root, default remote-logs in the current directory):
//!   <root>\<first seen YYYYMMDD-HHMMSS>_<session>\
//!       hello.txt  (kind hello, overwritten)   log.txt, prof.txt  (appended)
//!       meta.txt   (first seen, last seen, peer, requests, bytes per kind)   seqs.txt  (stored seqs, for restarts)
//!   <root>\index.tsv   one line per session: dir, first seen, last seen, device, build (from the hello lines)
//!   <root>\latest.txt  the newest session's directory
//! Files are opened, written and closed per request (never held open), so they can be read while the server runs.
//!
//! Updates for the app (the phone fetches new builds here; scripts/build.sh --publish puts them here):
//!   GET  /v1/apk/latest    -> 200 "commit=..\nsize=..\nsha256=..\nbuild_id=..\n" from <apk dir>\latest.txt, 404 if none
//!   GET  /v1/apk/download  -> <apk dir>\latest.apk, streamed in 1 MB chunks; "Range: bytes=N-" resumes (206)
//!   GET  /v1/core/latest   -> 200 "sha256=..\nsize=..\ngz_size=..\ncommit=..\n" from <apk dir>\core.txt, 404 if none
//!   GET  /v1/core/download -> <apk dir>\latest-core.so.gz (the player's own game core, gzipped), as the APK
//!   (--apk-dir, default apk in the current directory; scripts/build.sh --publish writes to BO2_APK_DIR, so the two
//!   must name the same directory)
//! Client modes for testing from the PC:
//!   bo2-logd --send <url> <session> <kind> <file> [seq]
//!   bo2-logd --ping <url>

use std::collections::{HashMap, HashSet};
use std::fs::{self, OpenOptions};
use std::io::{self, BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::net::{Shutdown, TcpListener, TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};
use std::time::Duration;

const MAX_BODY: usize = 4 << 20;
const MAX_HEAD: usize = 16 << 10;
const MAX_HEADERS: usize = 100;
const MAX_CONNS: usize = 64;
const IO_TIMEOUT: Duration = Duration::from_secs(30);
/// Loopback unless told otherwise: pass the PC's tailnet address with --bind (or BO2_LOGD_BIND) to reach the phone.
const DEFAULT_BIND: &str = "127.0.0.1:8764";
const KINDS: [&str; 3] = ["hello", "log", "prof"];
const KIND_FILES: [&str; 3] = ["hello.txt", "log.txt", "prof.txt"];
const APK_CHUNK: usize = 1 << 20;
/// Where scripts/build.sh --publish leaves latest.apk and latest.txt (--apk-dir).
static APK_DIR: OnceLock<PathBuf> = OnceLock::new();

// ---- Local time, without a crate --------------------------------------------------------------------------------------

struct Stamp {
    y: i64,
    mo: u32,
    d: u32,
    h: u32,
    mi: u32,
    s: u32,
    ms: u32,
}

impl Stamp {
    fn compact(&self) -> String {
        format!("{:04}{:02}{:02}-{:02}{:02}{:02}", self.y, self.mo, self.d, self.h, self.mi, self.s)
    }
    fn iso(&self) -> String {
        format!(
            "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
            self.y, self.mo, self.d, self.h, self.mi, self.s, self.ms
        )
    }
}

#[cfg(windows)]
fn now() -> Stamp {
    #[repr(C)]
    struct SystemTime {
        y: u16,
        mo: u16,
        dow: u16,
        d: u16,
        h: u16,
        mi: u16,
        s: u16,
        ms: u16,
    }
    #[link(name = "kernel32")]
    extern "system" {
        fn GetLocalTime(t: *mut SystemTime);
    }
    let mut t = SystemTime { y: 0, mo: 0, dow: 0, d: 0, h: 0, mi: 0, s: 0, ms: 0 };
    // SAFETY: GetLocalTime fills the SYSTEMTIME it is given and cannot fail.
    unsafe { GetLocalTime(&mut t) };
    let _ = t.dow;
    Stamp { y: t.y as i64, mo: t.mo as u32, d: t.d as u32, h: t.h as u32, mi: t.mi as u32, s: t.s as u32, ms: t.ms as u32 }
}

#[cfg(not(windows))]
fn now() -> Stamp {
    // UTC (a test build off Windows); civil date from days since 1970 (H. Hinnant's algorithm).
    let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
    let secs = t.as_secs() as i64;
    let (days, rem) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let mo = (if mp < 10 { mp + 3 } else { mp - 9 }) as u32;
    let y = yoe + era * 400 + if mo <= 2 { 1 } else { 0 };
    Stamp { y, mo, d, h: (rem / 3_600) as u32, mi: (rem / 60 % 60) as u32, s: (rem % 60) as u32, ms: t.subsec_millis() }
}

// ---- Sessions on disk -------------------------------------------------------------------------------------------------

#[derive(Default)]
struct Session {
    dir: String,
    first: String,
    last: String,
    peer: String,
    requests: u64,
    bytes: [u64; 3],
    seqs: [HashSet<u64>; 3],
    device: String,
    build: String,
}

struct Store {
    root: PathBuf,
    sessions: HashMap<String, Session>,
}

enum Outcome {
    Stored,
    Duplicate,
}

fn valid_session(s: &str) -> bool {
    !s.is_empty() && s.len() <= 96 && s.bytes().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'-'))
}

fn kind_index(s: &str) -> Option<usize> {
    KINDS.iter().position(|k| k.eq_ignore_ascii_case(s.trim()))
}

/// The session of a directory named `YYYYMMDD-HHMMSS_<session>`.
fn session_of_dir(name: &str) -> Option<String> {
    let b = name.as_bytes();
    if b.len() < 17 || b[8] != b'-' || b[15] != b'_' {
        return None;
    }
    if !b[..8].iter().chain(&b[9..15]).all(u8::is_ascii_digit) {
        return None;
    }
    let id = &name[16..];
    valid_session(id).then(|| id.to_string())
}

/// `device=` and `build=` from hello lines: a line starting with the key takes the rest of the line, otherwise a
/// whitespace-separated `key=value` token anywhere in a line.
fn parse_hello(text: &str, s: &mut Session) {
    for line in text.lines() {
        let line = line.trim();
        for (key, slot) in [("device=", &mut s.device), ("build=", &mut s.build)] {
            if let Some(v) = line.strip_prefix(key) {
                *slot = v.trim().to_string();
            } else if let Some(v) = line.split_whitespace().find_map(|t| t.strip_prefix(key)) {
                *slot = v.to_string();
            }
        }
    }
}

fn tsv(s: &str) -> String {
    s.replace(['\t', '\r', '\n'], " ")
}

fn append(path: &Path, data: &[u8]) -> io::Result<()> {
    let mut f = OpenOptions::new().create(true).append(true).open(path)?;
    f.write_all(data)?;
    f.flush()
}

/// Write a whole small file: to a temporary beside it, then renamed over it, so a reader never sees half of it. If the
/// rename is refused (a reader holds the file without delete sharing), write it in place.
fn write_replace(path: &Path, data: &[u8]) -> io::Result<()> {
    let tmp = path.with_extension("tmp");
    fs::write(&tmp, data)?;
    if fs::rename(&tmp, path).is_err() {
        let _ = fs::remove_file(&tmp);
        fs::write(path, data)?;
    }
    Ok(())
}

fn write_meta(dir: &Path, s: &Session) -> io::Result<()> {
    let m = format!(
        "session_dir={}\nfirst_seen={}\nlast_seen={}\npeer={}\nrequests={}\nbytes_hello={}\nbytes_log={}\nbytes_prof={}\n\
         device={}\nbuild={}\n",
        s.dir, s.first, s.last, s.peer, s.requests, s.bytes[0], s.bytes[1], s.bytes[2], tsv(&s.device), tsv(&s.build)
    );
    write_replace(&dir.join("meta.txt"), m.as_bytes())
}

impl Store {
    /// The sessions already under `root` (a restart keeps appending to them, and remembers their seqs).
    fn load(root: PathBuf) -> io::Result<Store> {
        fs::create_dir_all(&root)?;
        let mut sessions: HashMap<String, Session> = HashMap::new();
        for entry in fs::read_dir(&root)?.flatten() {
            if !entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                continue;
            }
            let name = entry.file_name().to_string_lossy().into_owned();
            let Some(id) = session_of_dir(&name) else { continue };
            let dir = root.join(&name);
            let mut s = Session { dir: name, ..Default::default() };
            if let Ok(meta) = fs::read_to_string(dir.join("meta.txt")) {
                for line in meta.lines() {
                    let Some((k, v)) = line.split_once('=') else { continue };
                    match k {
                        "first_seen" => s.first = v.to_string(),
                        "last_seen" => s.last = v.to_string(),
                        "peer" => s.peer = v.to_string(),
                        "requests" => s.requests = v.parse().unwrap_or(0),
                        "bytes_hello" => s.bytes[0] = v.parse().unwrap_or(0),
                        "bytes_log" => s.bytes[1] = v.parse().unwrap_or(0),
                        "bytes_prof" => s.bytes[2] = v.parse().unwrap_or(0),
                        _ => {}
                    }
                }
            }
            if let Ok(seqs) = fs::read_to_string(dir.join("seqs.txt")) {
                for line in seqs.lines() {
                    let mut it = line.split_whitespace();
                    if let (Some(k), Some(n)) = (it.next().and_then(kind_index), it.next().and_then(|n| n.parse().ok())) {
                        s.seqs[k].insert(n);
                    }
                }
            }
            if let Ok(hello) = fs::read_to_string(dir.join("hello.txt")) {
                parse_hello(&hello, &mut s);
            }
            // Two directories for one session (should not happen): the newest wins.
            if sessions.get(&id).is_none_or(|old| old.dir < s.dir) {
                sessions.insert(id, s);
            }
        }
        Ok(Store { root, sessions })
    }

    fn ingest(&mut self, peer: &str, id: &str, kind: usize, seq: Option<u64>, body: &[u8]) -> io::Result<Outcome> {
        let stamp = now();
        let iso = stamp.iso();
        if !self.sessions.contains_key(id) {
            let dir = format!("{}_{}", stamp.compact(), id);
            fs::create_dir_all(self.root.join(&dir))?;
            self.sessions.insert(id.to_string(), Session { dir, first: iso.clone(), ..Default::default() });
        }
        let root = self.root.clone();
        let Some(s) = self.sessions.get_mut(id) else {
            return Err(io::Error::other("session vanished"));
        };
        let dir = root.join(&s.dir);
        fs::create_dir_all(&dir)?; // in case it was deleted while the server ran
        s.last = iso;
        s.peer = peer.to_string();
        s.requests += 1;
        if let Some(n) = seq {
            if s.seqs[kind].contains(&n) {
                write_meta(&dir, s)?;
                return Ok(Outcome::Duplicate);
            }
        }
        let mut text = String::from_utf8_lossy(body).into_owned();
        if !text.is_empty() && !text.ends_with('\n') {
            text.push('\n');
        }
        if kind == 0 {
            write_replace(&dir.join(KIND_FILES[0]), text.as_bytes())?;
            parse_hello(&text, s);
        } else {
            append(&dir.join(KIND_FILES[kind]), text.as_bytes())?;
        }
        s.bytes[kind] += body.len() as u64;
        if let Some(n) = seq {
            s.seqs[kind].insert(n);
            append(&dir.join("seqs.txt"), format!("{} {}\n", KINDS[kind], n).as_bytes())?;
        }
        write_meta(&dir, s)?;
        self.write_index()?;
        Ok(Outcome::Stored)
    }

    fn write_index(&self) -> io::Result<()> {
        let mut v: Vec<&Session> = self.sessions.values().collect();
        v.sort_by(|a, b| a.dir.cmp(&b.dir));
        let mut out = String::from("dir\tfirst_seen\tlast_seen\tdevice\tbuild\n");
        for s in &v {
            out += &format!("{}\t{}\t{}\t{}\t{}\n", s.dir, s.first, s.last, tsv(&s.device), tsv(&s.build));
        }
        write_replace(&self.root.join("index.tsv"), out.as_bytes())?;
        // The newest session by first-seen time (to the millisecond; two sessions can start in the same second).
        if let Some(newest) = v.iter().max_by(|a, b| (a.first.as_str(), &a.dir).cmp(&(b.first.as_str(), &b.dir))) {
            write_replace(&self.root.join("latest.txt"), format!("{}\n", newest.dir).as_bytes())?;
        }
        Ok(())
    }
}

fn lock(m: &Mutex<Store>) -> MutexGuard<'_, Store> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

// ---- HTTP/1.1, the little this needs ----------------------------------------------------------------------------------

struct Request {
    method: String,
    path: String,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
    keep_alive: bool,
}

enum ReadError {
    Eof,
    Io(io::Error),
    Bad(u16, &'static str),
}

fn header<'a>(h: &'a [(String, String)], name: &str) -> Option<&'a str> {
    h.iter().find(|(k, _)| k.as_str() == name).map(|(_, v)| v.as_str())
}

fn trim_crlf(b: &[u8]) -> &[u8] {
    let mut e = b.len();
    while e > 0 && (b[e - 1] == b'\n' || b[e - 1] == b'\r') {
        e -= 1;
    }
    &b[..e]
}

/// One line of the head, counted against what is left of the head's budget.
fn read_line_limited<R: BufRead>(r: &mut R, budget: &mut usize, buf: &mut Vec<u8>) -> Result<usize, ReadError> {
    buf.clear();
    let n = r.by_ref().take(*budget as u64 + 1).read_until(b'\n', buf).map_err(ReadError::Io)?;
    if n > *budget {
        return Err(ReadError::Bad(431, "request head over 16 KB"));
    }
    *budget -= n;
    Ok(n)
}

fn read_request<R: BufRead>(r: &mut R, out: &mut TcpStream) -> Result<Request, ReadError> {
    let mut budget = MAX_HEAD;
    let mut buf = Vec::new();
    let line = loop {
        if read_line_limited(r, &mut budget, &mut buf)? == 0 {
            return Err(ReadError::Eof);
        }
        let l = trim_crlf(&buf);
        if !l.is_empty() {
            break String::from_utf8_lossy(l).into_owned();
        }
    };
    let mut parts = line.split_whitespace();
    let (Some(method), Some(target), Some(version)) = (parts.next(), parts.next(), parts.next()) else {
        return Err(ReadError::Bad(400, "bad request line"));
    };
    if !version.starts_with("HTTP/1.") {
        return Err(ReadError::Bad(505, "HTTP/1.x only"));
    }
    let mut headers: Vec<(String, String)> = Vec::new();
    loop {
        if read_line_limited(r, &mut budget, &mut buf)? == 0 {
            return Err(ReadError::Bad(400, "truncated request head"));
        }
        let l = trim_crlf(&buf);
        if l.is_empty() {
            break;
        }
        let l = String::from_utf8_lossy(l);
        let Some((k, v)) = l.split_once(':') else { return Err(ReadError::Bad(400, "bad header line")) };
        headers.push((k.trim().to_ascii_lowercase(), v.trim().to_string()));
        if headers.len() > MAX_HEADERS {
            return Err(ReadError::Bad(431, "too many headers"));
        }
    }
    let conn = header(&headers, "connection").map(|v| v.to_ascii_lowercase());
    let keep_alive = if version == "HTTP/1.0" {
        conn.as_deref() == Some("keep-alive")
    } else {
        conn.as_deref() != Some("close")
    };
    if header(&headers, "transfer-encoding").is_some() {
        return Err(ReadError::Bad(501, "chunked bodies are not supported: send Content-Length"));
    }
    let len = match header(&headers, "content-length") {
        Some(v) => v.parse::<usize>().map_err(|_| ReadError::Bad(400, "bad Content-Length"))?,
        None if method == "POST" => return Err(ReadError::Bad(411, "Content-Length required")),
        None => 0,
    };
    let expect_continue = header(&headers, "expect").is_some_and(|v| v.eq_ignore_ascii_case("100-continue"));
    if len > MAX_BODY {
        // A client that asked to continue has not sent the body: refuse at once. Otherwise read and drop it first
        // (up to 64 MB), or the close resets the connection under the client and it never sees the 413.
        if !expect_continue {
            let _ = io::copy(&mut r.by_ref().take(len.min(64 << 20) as u64), &mut io::sink());
        }
        return Err(ReadError::Bad(413, "body over 4 MB"));
    }
    if len > 0 && expect_continue {
        let _ = out.write_all(b"HTTP/1.1 100 Continue\r\n\r\n");
    }
    let mut body = vec![0u8; len];
    r.read_exact(&mut body).map_err(ReadError::Io)?;
    let path = target.split('?').next().unwrap_or("").to_string();
    Ok(Request { method: method.to_string(), path, headers, body, keep_alive })
}

fn respond(out: &mut TcpStream, status: u16, body: &[u8], close: bool) -> io::Result<()> {
    let reason = match status {
        200 => "OK",
        204 => "No Content",
        206 => "Partial Content",
        416 => "Range Not Satisfiable",
        400 => "Bad Request",
        404 => "Not Found",
        405 => "Method Not Allowed",
        411 => "Length Required",
        413 => "Payload Too Large",
        431 => "Request Header Fields Too Large",
        500 => "Internal Server Error",
        501 => "Not Implemented",
        503 => "Service Unavailable",
        505 => "HTTP Version Not Supported",
        _ => "",
    };
    let mut head = format!("HTTP/1.1 {status} {reason}\r\nServer: bo2-logd\r\n");
    if status != 204 {
        head += &format!("Content-Type: text/plain; charset=utf-8\r\nContent-Length: {}\r\n", body.len());
    }
    head += if close { "Connection: close\r\n\r\n" } else { "Connection: keep-alive\r\n\r\n" };
    out.write_all(head.as_bytes())?;
    if status != 204 {
        out.write_all(body)?;
    }
    out.flush()
}

fn log_line(peer: &str, method: &str, path: &str, status: u16, info: &str) {
    let mut o = io::stdout().lock();
    let _ = writeln!(o, "{} {} {} {} {} {}", now().iso(), peer, method, path, status, info);
    let _ = o.flush();
}

fn ingest_route(req: &Request, peer: &str, store: &Mutex<Store>) -> (u16, Vec<u8>, String) {
    let h = &req.headers;
    let Some(id) = header(h, "x-bo2-session").filter(|s| valid_session(s)) else {
        return (400, b"missing or invalid X-Bo2-Session (1-96 of A-Z a-z 0-9 . _ -)\n".to_vec(), "bad session".into());
    };
    let Some(kind) = header(h, "x-bo2-kind").and_then(kind_index) else {
        return (400, b"X-Bo2-Kind must be hello, log or prof\n".to_vec(), format!("session={id} bad kind"));
    };
    let seq = match header(h, "x-bo2-seq") {
        None => None,
        Some(v) => match v.parse::<u64>() {
            Ok(n) => Some(n),
            Err(_) => return (400, b"bad X-Bo2-Seq\n".to_vec(), format!("session={id} bad seq")),
        },
    };
    let info = format!(
        "session={id} kind={} seq={} bytes={}",
        KINDS[kind],
        seq.map_or_else(|| "-".to_string(), |n| n.to_string()),
        req.body.len()
    );
    match lock(store).ingest(peer, id, kind, seq, &req.body) {
        Ok(Outcome::Stored) => (204, Vec::new(), info),
        Ok(Outcome::Duplicate) => (204, Vec::new(), info + " duplicate"),
        Err(e) => (500, format!("store: {e}\n").into_bytes(), format!("{info} error: {e}")),
    }
}

fn apk_dir() -> PathBuf {
    APK_DIR.get().cloned().unwrap_or_else(|| PathBuf::from(default_apk_dir()))
}

/// What scripts/build.sh --publish leaves in the build directory: the APK, and the player's game core (gzipped; it
/// is built from their own copy of the game and only ever served to their own phone, on their own tailnet).
struct Published {
    info: &'static str,
    file: &'static str,
    mime: &'static str,
}
const APK: Published = Published { info: "latest.txt", file: "latest.apk", mime: "application/vnd.android.package-archive" };
const CORE: Published = Published { info: "core.txt", file: "latest-core.so.gz", mime: "application/gzip" };

/// The info file as published, answered only when its file is there too.
fn published_latest(p: &Published) -> (u16, Vec<u8>, String) {
    let dir = apk_dir();
    match (fs::read(dir.join(p.info)), fs::metadata(dir.join(p.file))) {
        (Ok(text), Ok(_)) => (200, text, String::new()),
        _ => (404, b"no build published (scripts/build.sh --publish)\n".to_vec(), String::new()),
    }
}

/// The runtime switches the app applies at launch (Bo2App.java), from env.txt beside the APK directory
/// (env.txt in the parent of --apk-dir): NAME=VALUE lines, '#' comments. A missing file is an empty answer,
/// so the app's own defaults stand. This is how an A/B runs on the phone without adb or a new build.
fn env_txt() -> (u16, Vec<u8>, String) {
    let path = apk_dir().parent().map(|p| p.join("env.txt")).unwrap_or_else(|| PathBuf::from("env.txt"));
    match fs::read(&path) {
        Ok(text) => (200, text, String::new()),
        Err(_) => (200, Vec::new(), String::new()),
    }
}

/// "bytes=N-" -> N. Anything else (several ranges, a suffix, an end) is not a resume and gets the whole file.
fn range_start(h: &[(String, String)]) -> Option<u64> {
    let v = header(h, "range")?.trim();
    let n = v.strip_prefix("bytes=")?.strip_suffix('-')?;
    n.trim().parse().ok()
}

/// Streams a published file in 1 MB chunks (never whole in memory), whole or from a resume point. Writes its own
/// response, after which the connection closes. Returns the status and the log line's details.
fn published_download(p: &Published, req: &Request, out: &mut TcpStream) -> (u16, String) {
    let path = apk_dir().join(p.file);
    let mut file = match fs::File::open(&path) {
        Ok(f) => f,
        Err(_) => {
            let _ = respond(out, 404, b"no build published (scripts/build.sh --publish)\n", true);
            return (404, String::new());
        }
    };
    let len = file.metadata().map(|m| m.len()).unwrap_or(0);
    let start = range_start(&req.headers).unwrap_or(0);
    if start > len || (start == len && start > 0) {
        let head = format!(
            "HTTP/1.1 416 Range Not Satisfiable\r\nServer: bo2-logd\r\nContent-Range: bytes */{len}\r\n\
             Content-Length: 0\r\nConnection: close\r\n\r\n"
        );
        let _ = out.write_all(head.as_bytes());
        return (416, format!("from={start} size={len}"));
    }
    let partial = start > 0;
    let status = if partial { 206 } else { 200 };
    let mut head = format!(
        "HTTP/1.1 {status} {}\r\nServer: bo2-logd\r\nContent-Type: {}\r\n\
         Accept-Ranges: bytes\r\nContent-Length: {}\r\n",
        if partial { "Partial Content" } else { "OK" },
        p.mime,
        len - start
    );
    if partial {
        head += &format!("Content-Range: bytes {start}-{}/{len}\r\n", len - 1);
    }
    head += "Connection: close\r\n\r\n";
    if out.write_all(head.as_bytes()).is_err() {
        return (status, format!("from={start} size={len} sent=0 (client gone)"));
    }
    if partial && file.seek(SeekFrom::Start(start)).is_err() {
        return (500, format!("from={start} size={len} seek failed"));
    }
    let mut buf = vec![0u8; APK_CHUNK];
    let mut sent: u64 = 0;
    let mut note = "";
    loop {
        let n = match file.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(_) => {
                note = " (read error)";
                break;
            }
        };
        if out.write_all(&buf[..n]).is_err() {
            note = " (client gone)";
            break;
        }
        sent += n as u64;
    }
    let _ = out.flush();
    (status, format!("from={start} size={len} sent={sent}{note}"))
}

fn route(req: &Request, peer: &str, store: &Mutex<Store>) -> (u16, Vec<u8>, String) {
    match (req.method.as_str(), req.path.as_str()) {
        ("GET", "/v1/ping") => (200, b"bo2-logd ok\n".to_vec(), String::new()),
        ("GET", "/v1/apk/latest") => published_latest(&APK),
        ("GET", "/v1/core/latest") => published_latest(&CORE),
        (_, "/v1/core/latest") | (_, "/v1/core/download") => (405, b"GET only\n".to_vec(), String::new()),
        ("GET", "/v1/env") => env_txt(),
        (_, "/v1/env") => (405, b"GET only\n".to_vec(), String::new()),
        (_, "/v1/apk/latest") | (_, "/v1/apk/download") => (405, b"GET only\n".to_vec(), String::new()),
        (_, "/v1/ping") => (405, b"GET only\n".to_vec(), String::new()),
        ("POST", "/v1/ingest") => ingest_route(req, peer, store),
        (_, "/v1/ingest") => (405, b"POST only\n".to_vec(), String::new()),
        _ => (404, b"not found\n".to_vec(), String::new()),
    }
}

fn handle_conn(stream: TcpStream, store: Arc<Mutex<Store>>) {
    let peer = stream.peer_addr().map(|a| a.to_string()).unwrap_or_else(|_| "?".into());
    let _ = stream.set_read_timeout(Some(IO_TIMEOUT));
    let _ = stream.set_write_timeout(Some(IO_TIMEOUT));
    let _ = stream.set_nodelay(true);
    let Ok(rd) = stream.try_clone() else { return };
    let mut reader = BufReader::new(rd);
    let mut out = stream;
    loop {
        match read_request(&mut reader, &mut out) {
            Ok(req) => {
                let download = match req.path.as_str() {
                    "/v1/apk/download" => Some(&APK),
                    "/v1/core/download" => Some(&CORE),
                    _ => None,
                };
                if let (true, Some(p)) = (req.method == "GET", download) {
                    let (status, info) = published_download(p, &req, &mut out);
                    log_line(&peer, &req.method, &req.path, status, &info);
                    break;
                }
                let (status, body, info) = route(&req, &peer, &store);
                log_line(&peer, &req.method, &req.path, status, &info);
                let close = !req.keep_alive;
                if respond(&mut out, status, &body, close).is_err() || close {
                    break;
                }
            }
            Err(ReadError::Eof) => break,
            Err(ReadError::Io(e)) => {
                if !matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut) {
                    log_line(&peer, "-", "-", 0, &format!("read error: {e}"));
                }
                break;
            }
            Err(ReadError::Bad(status, why)) => {
                log_line(&peer, "-", "-", status, why);
                let _ = respond(&mut out, status, format!("{why}\n").as_bytes(), true);
                break;
            }
        }
    }
    let _ = out.shutdown(Shutdown::Both);
}

/// Decrements the live-connection count when a connection's thread ends, panic or not.
struct Live(Arc<AtomicUsize>);
impl Drop for Live {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::SeqCst);
    }
}

// ---- Client modes (tests from the PC) ---------------------------------------------------------------------------------

fn parse_url(url: &str, default_path: &str) -> Option<(String, u16, String)> {
    let rest = url.strip_prefix("http://")?;
    let (hostport, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, ""),
    };
    let path = if path.is_empty() || path == "/" { default_path.to_string() } else { path.to_string() };
    let (host, port) = match hostport.rsplit_once(':') {
        Some((h, p)) => (h.to_string(), p.parse().ok()?),
        None => (hostport.to_string(), 80),
    };
    Some((host, port, path))
}

fn http_call(host: &str, port: u16, method: &str, path: &str, extra: &str, body: &[u8]) -> io::Result<(u16, String)> {
    let addr = (host, port)
        .to_socket_addrs()?
        .next()
        .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "no address"))?;
    let mut s = TcpStream::connect_timeout(&addr, Duration::from_secs(10))?;
    s.set_read_timeout(Some(IO_TIMEOUT))?;
    s.set_write_timeout(Some(IO_TIMEOUT))?;
    let head = format!(
        "{method} {path} HTTP/1.1\r\nHost: {host}:{port}\r\n{extra}Content-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    s.write_all(head.as_bytes())?;
    s.write_all(body)?;
    s.flush()?;
    let mut resp = Vec::new();
    s.read_to_end(&mut resp)?;
    let text = String::from_utf8_lossy(&resp).into_owned();
    let status = text.split_whitespace().nth(1).and_then(|c| c.parse().ok()).unwrap_or(0);
    let body = text.split_once("\r\n\r\n").map(|(_, b)| b.to_string()).unwrap_or_default();
    Ok((status, body))
}

fn client(mode: &str, a: &[String]) -> i32 {
    let (url, default_path) = match (mode, a.first()) {
        ("--ping", Some(u)) => (u, "/v1/ping"),
        ("--send", Some(u)) if a.len() >= 4 => (u, "/v1/ingest"),
        _ => {
            usage();
            return 2;
        }
    };
    let Some((host, port, path)) = parse_url(url, default_path) else {
        eprintln!("bad url {url} (http://host:port[/path])");
        return 2;
    };
    let result = if mode == "--ping" {
        http_call(&host, port, "GET", &path, "", &[])
    } else {
        let body = match fs::read(&a[3]) {
            Ok(b) => b,
            Err(e) => {
                eprintln!("cannot read {}: {e}", a[3]);
                return 2;
            }
        };
        let mut extra = format!("X-Bo2-Session: {}\r\nX-Bo2-Kind: {}\r\n", a[1], a[2]);
        if let Some(seq) = a.get(4) {
            extra += &format!("X-Bo2-Seq: {seq}\r\n");
        }
        http_call(&host, port, "POST", &path, &extra, &body)
    };
    match result {
        Ok((status, body)) => {
            println!("{status} {}", body.trim_end());
            if (200..300).contains(&status) {
                0
            } else {
                1
            }
        }
        Err(e) => {
            eprintln!("request failed: {e}");
            1
        }
    }
}

// ---- Main -------------------------------------------------------------------------------------------------------------

/// Both in the directory the server is started from, unless given (--root, --apk-dir).
fn default_root() -> String {
    "remote-logs".to_string()
}

fn default_apk_dir() -> String {
    "apk".to_string()
}

fn usage() {
    eprintln!(
        "bo2-logd: the bo2-Android app's log receiver\n\
         \n  bo2-logd [--bind ADDR:PORT] [--any] [--root DIR] [--apk-dir DIR]\n\
         \x20     --bind  listen address (default {DEFAULT_BIND}, or BO2_LOGD_BIND; give the PC's tailnet address)\n\
         \x20     --any   if that address cannot be bound, listen on 0.0.0.0 at the same port instead\n\
         \x20     --root  storage directory (default {})\n\
         \x20     --apk-dir  where scripts/build.sh --publish leaves latest.apk and latest.txt (default {})\n\
         \n  bo2-logd --ping <url>                                  e.g. http://100.64.0.1:8764\n\
         \x20 bo2-logd --send <url> <session> <hello|log|prof> <file> [seq]",
        default_root(),
        default_apk_dir()
    );
}

fn run(args: Vec<String>) -> i32 {
    let mut bind = std::env::var("BO2_LOGD_BIND").unwrap_or_else(|_| DEFAULT_BIND.to_string());
    let mut any = false;
    let mut root = default_root();
    let mut apk_dir_arg: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--bind" | "--root" | "--apk-dir" => {
                let Some(v) = args.get(i + 1) else {
                    usage();
                    return 2;
                };
                match args[i].as_str() {
                    "--bind" => bind = v.clone(),
                    "--root" => root = v.clone(),
                    _ => apk_dir_arg = Some(v.clone()),
                }
                i += 1;
            }
            "--any" => any = true,
            m @ ("--send" | "--ping") => return client(m, &args[i + 1..]),
            "-h" | "--help" => {
                usage();
                return 0;
            }
            other => {
                eprintln!("unknown argument {other}");
                usage();
                return 2;
            }
        }
        i += 1;
    }
    let _ = APK_DIR.set(PathBuf::from(apk_dir_arg.unwrap_or_else(default_apk_dir)));
    let store = match Store::load(PathBuf::from(&root)) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("cannot use {root}: {e}");
            return 1;
        }
    };
    let listener = match TcpListener::bind(&bind) {
        Ok(l) => l,
        Err(e) if any => {
            let port = bind.rsplit_once(':').and_then(|(_, p)| p.parse::<u16>().ok()).unwrap_or(8764);
            eprintln!("cannot listen on {bind} ({e}); --any: listening on 0.0.0.0:{port}");
            match TcpListener::bind(("0.0.0.0", port)) {
                Ok(l) => l,
                Err(e) => {
                    eprintln!("cannot listen on 0.0.0.0:{port}: {e}");
                    return 1;
                }
            }
        }
        Err(e) => {
            eprintln!("cannot listen on {bind}: {e} (is Tailscale up? --any falls back to 0.0.0.0)");
            return 1;
        }
    };
    let n = store.sessions.len();
    println!(
        "{} bo2-logd listening on {}, storing in {root} ({n} earlier session(s)), builds from {}",
        now().iso(),
        listener.local_addr().map(|a| a.to_string()).unwrap_or_default(),
        apk_dir().display()
    );
    let store = Arc::new(Mutex::new(store));
    let live = Arc::new(AtomicUsize::new(0));
    for conn in listener.incoming() {
        let Ok(mut stream) = conn else { continue };
        if live.load(Ordering::SeqCst) >= MAX_CONNS {
            let _ = stream.set_write_timeout(Some(Duration::from_secs(2)));
            let _ = respond(&mut stream, 503, b"busy\n", true);
            continue;
        }
        live.fetch_add(1, Ordering::SeqCst);
        let guard = Live(live.clone());
        let st = store.clone();
        let spawned = std::thread::Builder::new().name("bo2-logd-conn".into()).spawn(move || {
            let _guard = guard;
            handle_conn(stream, st);
        });
        if let Err(e) = spawned {
            log_line("-", "-", "-", 0, &format!("cannot start a connection thread: {e}"));
        }
    }
    0
}

fn main() {
    std::process::exit(run(std::env::args().skip(1).collect()));
}
