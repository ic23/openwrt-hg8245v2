#!/usr/bin/env python3
"""
General recursive reader for the exact legacy SquashFS 3.x + jro/LZMA
filesystem used by the HG8245 firmware dump.

Evidence basis:
  * firmware_current/extracted/rootfs.bin is SquashFS 3.1 on-disk format
    (vendor kernel/log calls it SquashFS 3.4) with the legacy jro LZMA magic.
  * inode metadata uses the packed v3 structs and directory metadata layout.
  * this image contains inode types 1..5 only after a complete metadata scan.

This tool is deliberately conservative: it validates the full inode table,
indexes all inode numbers, walks the directory tree, and only then extracts
regular files. It does not attempt unsupported SquashFS versions.
"""
from __future__ import annotations
import argparse, hashlib, lzma, os, stat, struct
from dataclasses import dataclass
from pathlib import Path

MAGIC_LZMA=b"shsq"
BLOCK_SIZE=1<<17
INVALID_FRAG=0xffffffff
INODE_START=0x8c7575
DIR_START=0x8c9d53
DIR_END=0x8ccc55
FRAG_INDEX=0x8ccf51

TYPE_NAMES={1:"dir",2:"file",3:"symlink",4:"blockdev",5:"chardev",6:"fifo",7:"socket",8:"ldir",9:"lreg"}

def ru16(b,o): return struct.unpack_from('<H',b,o)[0]
def ru32(b,o): return struct.unpack_from('<I',b,o)[0]
def ru64(b,o): return struct.unpack_from('<Q',b,o)[0]
def rs16(b,o): return struct.unpack_from('<h',b,o)[0]

def bitfield_word(b,o): return ru32(b,o)

def metadata_stream(img:bytes,start:int,end:int):
    out=bytearray(); blocks=[]; pos=start
    while pos<end:
        if pos+2>end: raise ValueError(f"metadata header truncated at {pos:#x}")
        hdr=ru16(img,pos); ln=hdr&0x7fff; nxt=pos+2+ln
        if nxt>end: raise ValueError(f"metadata block overruns end at {pos:#x}")
        payload=img[pos+2:nxt]
        if hdr&0x8000: dec=payload
        else:
            if payload[:5]!=b"\x5d\x00\x00\x01\x00":
                raise ValueError(f"unsupported legacy LZMA header at {pos:#x}: {payload[:8].hex()}")
            dec=lzma.decompress(payload,format=lzma.FORMAT_ALONE)
        blocks.append((pos,nxt,len(out),len(dec),hdr)); out.extend(dec); pos=nxt
    return bytes(out),blocks

@dataclass
class Inode:
    number:int
    typ:int
    off:int
    size:int
    start_block:int=0
    fragment:int=INVALID_FRAG
    fragment_offset:int=0
    nlink:int=0
    target:bytes=b""
    rdev:int=0
    raw_size:int=0

class FS:
    def __init__(self,img:bytes):
        if isinstance(img, (str, Path)):
            img=Path(img).read_bytes()
        self.img=img
        if img[:4]!=MAGIC_LZMA: raise ValueError(f"magic {img[:4].hex()} is not shsq")
        self.inode,self.iblocks=metadata_stream(img,INODE_START,DIR_START)
        self.directory,self.dblocks=metadata_stream(img,DIR_START,DIR_END)
        self.inodes={}
        self._parse_inodes()
        self.frags=self._parse_fragments()
        self.dir_block_map={x[0]:x for x in self.dblocks}

    def _parse_inodes(self):
        o=0; count=0
        while o<len(self.inode):
            if o+12>len(self.inode): raise ValueError(f"truncated inode at stream {o:#x}")
            first=ru32(self.inode,o); typ=first&0xf; ino=ru32(self.inode,o+8)
            if typ==1:
                if o+28>len(self.inode): raise ValueError(f"dir inode truncated {o:#x}")
                nlink=ru32(self.inode,o+12); w=ru32(self.inode,o+16)
                fsize=w&0x7ffff; off=(w>>19)&0x1fff; start=ru32(self.inode,o+20)
                sz=28
                rec=Inode(ino,typ,o,fsize,start_block=start,nlink=nlink,raw_size=sz)
            elif typ==2:
                if o+32>len(self.inode): raise ValueError(f"file inode truncated {o:#x}")
                start=ru64(self.inode,o+12); frag=ru32(self.inode,o+20); off=ru32(self.inode,o+24); fsize=ru32(self.inode,o+28)
                blocks=(fsize+BLOCK_SIZE-1)//BLOCK_SIZE if frag==INVALID_FRAG else fsize//BLOCK_SIZE
                sz=32+4*blocks
                rec=Inode(ino,typ,o,fsize,start_block=start,fragment=frag,fragment_offset=off,raw_size=sz)
            elif typ==3:
                nlink=ru32(self.inode,o+12); ssize=ru16(self.inode,o+16)
                sz=18+ssize
                rec=Inode(ino,typ,o,ssize,nlink=nlink,target=self.inode[o+18:o+18+ssize],raw_size=sz)
            elif typ in (4,5):
                nlink=ru32(self.inode,o+12); rdev=ru16(self.inode,o+16)
                sz=18
                rec=Inode(ino,typ,o,0,nlink=nlink,rdev=rdev,raw_size=sz)
            elif typ in (6,7):
                raise ValueError(f"unsupported extended inode type {typ} at {o:#x}")
            elif typ==8:
                raise ValueError(f"extended directory inode type 8 at {o:#x} not expected in this image")
            elif typ==9:
                raise ValueError(f"extended regular inode type 9 at {o:#x} not expected in this image")
            else:
                raise ValueError(f"unknown inode type {typ} at {o:#x}")
            if o+sz>len(self.inode): raise ValueError(f"inode overruns stream at {o:#x}: {sz}")
            if ino in self.inodes: raise ValueError(f"duplicate inode number {ino}")
            self.inodes[ino]=rec; o+=sz; count+=1
        if o!=len(self.inode): raise AssertionError('inode stream end mismatch')
        if count!=1383: raise ValueError(f"expected 1383 inodes, parsed {count}")

    def _parse_fragments(self):
        ptr=ru64(self.img,FRAG_INDEX)
        if ptr!=DIR_END: raise ValueError(f"fragment index points {ptr:#x}, expected {DIR_END:#x}")
        raw,_=metadata_stream(self.img,ptr,FRAG_INDEX)
        if len(raw)%16: raise ValueError('fragment table size not multiple of 16')
        return [(ru64(raw,i),ru32(raw,i+8)) for i in range(0,len(raw),16)]

    def dir_location(self,rec:Inode):
        # SquashFS3 directory inode start_block is a byte offset from directory_table_start.
        abs_block=DIR_START+rec.start_block
        blk=self.dir_block_map.get(abs_block)
        if blk is None: raise ValueError(f"directory inode {rec.number}: no metadata block at {abs_block:#x}")
        streamoff=blk[2]+((ru32(self.inode,rec.off+16)>>19)&0x1fff)
        # file_size is the low 19 bits of the same word
        return streamoff,rec.size

    def read_dir(self,rec:Inode):
        pos,total=self.dir_location(rec); end=pos+total; out=[]
        if end>len(self.directory):
            # Exact image quirk: the root directory advertises 3 bytes past the
            # last available directory-metadata byte. The parsed groups consume
            # exactly the available bytes; tolerate only this <=3-byte tail.
            over=end-len(self.directory)
            if over<=3:
                end=len(self.directory)
            else:
                raise ValueError(f"dir inode {rec.number} exceeds dir stream by {over} bytes")
        while pos<end:
            if end-pos < 9:
                if end-pos <= 3:
                    break
                raise ValueError(f"dir header truncated at {pos:#x}, remaining={end-pos}")
            w=ru32(self.directory,pos); count=(w&0xff)+1
            start_block=ru32(self.directory,pos+1)
            base_ino=ru32(self.directory,pos+5)
            pos+=9
            for _ in range(count):
                if pos+5>end: raise ValueError(f"dir entry truncated at {pos:#x}")
                w=ru32(self.directory,pos)
                local_off=w&0x1fff; typ=(w>>13)&7; name_len=((w>>16)&0xff)+1; delta=struct.unpack_from('<h',self.directory,pos+3)[0]
                if pos+5+name_len>end: raise ValueError(f"dir entry name overruns at {pos:#x}")
                name=self.directory[pos+5:pos+5+name_len]
                try: name_s=name.decode('utf-8')
                except UnicodeDecodeError: name_s=name.decode('latin-1')
                out.append((name_s,base_ino+delta,typ,local_off))
                pos += 5+name_len
        if pos!=end and end-pos>3: raise AssertionError(f'directory group end mismatch: remaining={end-pos}')
        return out

    def walk(self):
        # Root inode encoded in superblock is 0x25170b4d; v3 macro decodes to block+offset.
        root_encoded=ru64(self.img,0x2b)
        rb,ro=root_encoded>>16,root_encoded&0xffff
        abs_block=INODE_START+rb
        target_blk=next((x for x in self.iblocks if x[0]==abs_block),None)
        if target_blk is None: raise ValueError(f"root inode block {abs_block:#x} not found")
        root_stream=target_blk[2]+ro
        root_num=ru32(self.inode,root_stream+8)
        if root_num not in self.inodes: raise ValueError(f"root inode number {root_num} absent")
        if self.inodes[root_num].off!=root_stream: raise ValueError('root inode index mismatch')
        paths=[]; seen=set()
        def recdir(ino,path):
            if ino in seen: return
            seen.add(ino)
            rec=self.inodes[ino]
            if rec.typ!=1: raise ValueError(f"{path}: inode {ino} not directory")
            for name,child,dt,_ in self.read_dir(rec):
                if name in ('.','..'): continue
                if child not in self.inodes: raise ValueError(f"{path}/{name}: missing inode {child}")
                c=self.inodes[child]; p=path+'/'+name if path else name
                paths.append((p,c.number,c.typ,c.size))
                if c.typ==1: recdir(c.number,p)
        recdir(root_num,'')
        return root_num,paths

    def _read_blob(self,start_block:int, encoded:int):
        if encoded==0: return b''
        # SquashFS3 uses bit 24 as the *uncompressed* flag.
        uncompressed=bool(encoded & (1<<24)); compressed=not uncompressed
        n=encoded & 0x00ffffff
        payload=self.img[start_block:start_block+n]
        if len(payload)!=n: raise ValueError(f"data block short at {start_block:#x}")
        if compressed:
            if payload[:5]!=b"\x5d\x00\x00\x01\x00": raise ValueError(f"unexpected LZMA header at {start_block:#x}: {payload[:8].hex()}")
            return lzma.decompress(payload,format=lzma.FORMAT_ALONE)
        return payload

    def read_file(self,rec:Inode):
        if rec.typ!=2: raise ValueError('not regular file')
        out=bytearray()
        blocks=(rec.size+BLOCK_SIZE-1)//BLOCK_SIZE if rec.fragment==INVALID_FRAG else rec.size//BLOCK_SIZE
        pos=rec.off+32; data_pos=rec.start_block
        for i in range(blocks):
            enc=ru32(self.inode,pos+4*i)
            if enc==0:
                out.extend(b'\0'*min(BLOCK_SIZE,rec.size-len(out))); continue
            dec=self._read_blob(data_pos,enc); out.extend(dec); data_pos += (enc & 0x00ffffff if (enc&(1<<24)) else enc)
        if rec.fragment!=INVALID_FRAG:
            if rec.fragment>=len(self.frags): raise ValueError('fragment index out of range')
            fs,encsz=self.frags[rec.fragment]
            frag=self._read_blob(fs,encsz)
            out.extend(frag[rec.fragment_offset:rec.fragment_offset+(rec.size-len(out))])
        if len(out)<rec.size: raise ValueError(f"short file {rec.number}: {len(out)} < {rec.size}")
        return bytes(out[:rec.size])

    def extract_matches(self,outdir:Path,regex=None,contains=None):
        import re
        _,paths=self.walk(); selected=[]
        for p,ino,typ,size in paths:
            if typ!=2: continue
            ok=True
            if regex is not None: ok=bool(re.search(regex,p,re.I))
            if contains is not None: ok=contains.lower() in p.lower()
            if ok: selected.append((p,self.inodes[ino]))
        for p,rec in selected:
            data=self.read_file(rec); dst=outdir/p.lstrip('/'); dst.parent.mkdir(parents=True,exist_ok=True); dst.write_bytes(data)
        return selected

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('rootfs',type=Path); ap.add_argument('--list',action='store_true')
    ap.add_argument('--grep'); ap.add_argument('--extract',type=Path)
    ap.add_argument('--contains')
    args=ap.parse_args(); fs=FS(args.rootfs); root,paths=fs.walk()
    print(f"root_inode={root} inodes={len(fs.inodes)} inode_stream={len(fs.inode)} dir_stream={len(fs.directory)} fragments={len(fs.frags)}")
    print(f"inode_types={{{', '.join(f'{k}: {sum(1 for x in fs.inodes.values() if x.typ==k)}' for k in sorted(TYPE_NAMES) if any(x.typ==k for x in fs.inodes.values()))}}}")
    if args.list:
        for p,ino,typ,size in paths: print(f"{TYPE_NAMES.get(typ,str(typ))}\t{size}\tinode={ino}\t{p}")
    selected=fs.extract_matches(args.extract,regex=args.grep,contains=args.contains) if args.extract and (args.grep or args.contains) else []
    if selected:
        print(f"extracted={len(selected)}")
        for p,r in selected:
            dst=args.extract/p; print(f"{p}\tinode={r.number}\tsize={r.size}\tsha256={hashlib.sha256(dst.read_bytes()).hexdigest()}")

if __name__=='__main__': main()
