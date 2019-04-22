use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
use std::io;
use std::mem;
use std::io::{Error, ErrorKind, Read};

use Connection;
use Packet;
use Transport;

const SECTOR_SIZE: u32 = 512;

#[allow(dead_code)]
enum Opcode {
    Get = 0x00,
    Set = 0x01,
}

#[derive(Debug, Default)]
#[repr(packed)]
struct PacketHeader {
    pub magic: u16,
    pub opcode: u16,
    pub req_handle: u64,
    pub lba: u64,
    pub lba_count: u32,
}

impl PacketHeader {
    fn write<W: io::Write>(self, writer: &mut W) -> io::Result<()> {
        writer.write_u16::<BigEndian>(self.magic)?;
        writer.write_u16::<BigEndian>(self.opcode)?;
        writer.write_u64::<BigEndian>(self.req_handle)?;
        writer.write_u64::<BigEndian>(self.lba)?;
        writer.write_u32::<BigEndian>(self.lba_count)?;
        return Ok(());
    }

    fn read<R: io::Read>(reader: &mut R) -> io::Result<PacketHeader> {
        let magic = reader.read_u16::<BigEndian>()?;
        if magic != mem::size_of::<PacketHeader>() as u16 {
            return Err(Error::new(
                ErrorKind::Other,
                format!("Bad magic number in response header: {}", magic),
            ));
        }
        let header = PacketHeader {
            magic: magic,
            opcode: reader.read_u16::<BigEndian>()?,
            req_handle: reader.read_u64::<BigEndian>()?,
            lba: reader.read_u64::<BigEndian>()?,
            lba_count: reader.read_u32::<BigEndian>()?,
        };
        return Ok(header);
    }
}

pub static NVALUES: u64 = 1000000;
static PCT_SET: u64 = 2; // out of 1000
static VALUE_SIZE: usize = 2;
static KEY_SIZE: usize = 20;

#[inline(always)]
fn write_key(buf: &mut Vec<u8>, key: u64) {
    let mut pushed = 0;
    let mut k = key;
    loop {
        buf.push(48 + (k % 10) as u8);
        k /= 10;
        pushed += 1;
        if k == 0 {
            break;
        }
    }
    for _ in pushed..KEY_SIZE {
        buf.push('A' as u8);
    }
}

static UDP_HEADER: &'static [u8] = &[0, 0, 0, 0, 0, 1, 0, 0];

#[derive(Copy, Clone, Debug)]
pub struct ReflexProtocol;

impl ReflexProtocol {
    pub fn set_request(key: u64, opaque: u32, buf: &mut Vec<u8>, tport: Transport) {
        if let Transport::Udp = tport {
            buf.extend_from_slice(UDP_HEADER);
        }

        PacketHeader {
            magic: mem::size_of::<PacketHeader>() as u16,
            opcode: Opcode::Set as u16,
            req_handle: 0 as u64,
            lba: key as u64,
            lba_count: 1 as u32,
            ..Default::default()
        }
        .write(buf)
        .unwrap();

        for i in 0..VALUE_SIZE {
            buf.push((((key * i as u64) >> (i % 4)) & 0xff) as u8);
        }
    }

    pub fn gen_request(i: usize, p: &Packet, buf: &mut Vec<u8>, tport: Transport) {
        // Use first 32 bits of randomness to determine if this is a SET or GET req
        let low32 = p.randomness & 0xffffffff;
        let key = (p.randomness >> 32) % NVALUES;

        if low32 % 1000 < PCT_SET {
            ReflexProtocol::set_request(key, i as u32, buf, tport);
            return;
        }

        if let Transport::Udp = tport {
            buf.extend_from_slice(UDP_HEADER);
        }

        PacketHeader {
            magic: mem::size_of::<PacketHeader>() as u16,
            opcode: Opcode::Get as u16,
            req_handle: 0 as u64,
            lba: key as u64,
            lba_count: 1 as u32,
            ..Default::default()
        }
        .write(buf)
        .unwrap();
    }

    pub fn read_response(
        mut sock: &Connection,
        tport: Transport,
        scratch: &mut [u8],
    ) -> io::Result<usize> {
        let hdr = match tport {
            Transport::Udp => {
                let len = sock.read(&mut scratch[..32])?;
                if len == 0 {
                    return Err(Error::new(ErrorKind::UnexpectedEof, "eof"));
                }
                if len < 8 {
                    return Err(Error::new(
                        ErrorKind::Other,
                        format!("Short packet received: {} bytes", len),
                    ));
                }
                PacketHeader::read(&mut &scratch[8..])?
            }
            Transport::Tcp => {
                sock.read_exact(&mut scratch[..24])?;
                let hdr = PacketHeader::read(&mut &scratch[..])?;
                sock.read_exact(&mut scratch[..SECTOR_SIZE as usize])?;
                hdr
            }
        };

        // if hdr.vbucket_id_or_status != ResponseStatus::NoError as u16 {
        //     return Err(Error::new(
        //         ErrorKind::Other,
        //         format!("Not NoError {}", hdr.vbucket_id_or_status),
        //     ));
        // }
        Ok((hdr.lba_count * SECTOR_SIZE) as usize)
    }
}
