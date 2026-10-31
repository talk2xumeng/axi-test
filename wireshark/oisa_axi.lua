-- Wireshark Lua dissector for E100c OISA 10b frames (EtherType 0x88EE).
-- Field layout follows eth_header_parser.py: the first 12 bytes are OISA fields, not MAC addresses.
-- Install: copy to the Wireshark personal plugins folder, or: wireshark -X lua_script:oisa_axi.lua file.pcap

local oisa = Proto("oisa", "OISA 10b / AXI over Ethernet")
local eth_dissector = Dissector.get("eth_withoutfcs")

local FT_NAMES = { [0] = "write", [1] = "write(strb)", [2] = "write response", [3] = "read", [4] = "read response" }
local BEAT = 64

local f = {
    version = ProtoField.uint8("oisa.version", "Version", base.DEC, nil, 0xE0),
    vc = ProtoField.uint8("oisa.vc", "Virtual channel", base.DEC, nil, 0x1C),
    format = ProtoField.uint8("oisa.format", "Format", base.DEC, nil, 0xE0),
    pkt_type = ProtoField.uint8("oisa.packet_type", "Packet type", base.DEC, nil, 0x1F),
    length = ProtoField.uint16("oisa.length", "Length", base.DEC, nil, 0x1FFF),
    tag = ProtoField.uint16("oisa.tag", "Tag ID", base.DEC),
    dest = ProtoField.uint16("oisa.dest", "Dest ID", base.DEC, nil, 0x03FF),
    pack = ProtoField.uint16("oisa.packing_num", "Packing num", base.DEC, nil, 0xFC00),
    src = ProtoField.uint16("oisa.source", "Source ID", base.DEC, nil, 0x03FF),
    etype = ProtoField.uint16("oisa.ethertype", "EtherType", base.HEX),
    tail = ProtoField.uint16("oisa.tail", "Type / R / signal / locator", base.HEX),
    ft = ProtoField.uint8("oisa.frame_type", "Frame type", base.DEC, FT_NAMES),
    id = ProtoField.uint16("oisa.axi_id", "AXI ID", base.DEC),
    beats = ProtoField.uint8("oisa.beats", "Beats", base.DEC),
    wfull = ProtoField.uint8("oisa.write_full", "Write full", base.HEX),
    port = ProtoField.uint8("oisa.port_id", "User port ID", base.DEC),
    tsn = ProtoField.uint16("oisa.tsn", "User TSN", base.DEC),
    size = ProtoField.uint8("oisa.size", "Size", base.DEC),
    bresp = ProtoField.uint8("oisa.bresp", "BRESP", base.DEC),
    rresp = ProtoField.uint8("oisa.rresp", "RRESP", base.DEC),
    data = ProtoField.bytes("oisa.data", "Data"),
    pad = ProtoField.bytes("oisa.padding", "Padding"),
}
oisa.fields = f

local function bits(v, hi, lo)
    return math.floor(v / 2 ^ lo) % 2 ^ (hi - lo + 1)
end

-- Returns the transaction length, or nil when the bytes at off are not a transaction.
local function dissect_txn(tvb, off, tree)
    local remain = tvb:len() - off
    if remain < 4 then return nil end
    local w0 = tvb(off, 4):uint()
    if w0 == 0 then return nil end                 -- real transactions are never all zero
    local ft = bits(w0, 31, 28)
    local id = bits(w0, 27, 19)
    local len, name

    if ft == 0 or ft == 1 then
        if remain < 16 then return nil end
        local beats = bits(w0, 18, 17) + 1
        local w1 = tvb(off + 4, 4):uint()
        len = 16 + beats * (ft == 1 and (8 + BEAT) or BEAT)
        if len > remain then len = remain end
        local t = tree:add(oisa, tvb(off, len), string.format("Write: ID %d, %d beats", math.floor(id), math.floor(beats)))
        t:add(f.ft, tvb(off, 1), ft)
        t:add(f.id, tvb(off, 2), id)
        t:add(f.beats, tvb(off + 1, 1), beats)
        t:add(f.wfull, tvb(off + 1, 2), bits(w0, 16, 13))
        t:add(f.port, tvb(off + 3, 1), bits(w0, 7, 5))
        t:add(f.tsn, tvb(off + 3, 3), bits(w0, 4, 0) * 2048 + bits(w1, 31, 21))
        t:add(f.size, tvb(off + 15, 1), bits(tvb(off + 15, 1):uint(), 2, 0))
        if len > 16 then t:add(f.data, tvb(off + 16, len - 16)) end
        name = "wr"
    elseif ft == 3 then
        if remain < 16 then return nil end
        local w1 = tvb(off + 4, 4):uint()
        local w3 = tvb(off + 12, 4):uint()
        len = 16
        local beats = bits(w3, 4, 3) + 1
        local t = tree:add(oisa, tvb(off, len), string.format("Read: ID %d, %d beats", math.floor(id), math.floor(beats)))
        t:add(f.ft, tvb(off, 1), ft)
        t:add(f.id, tvb(off, 2), id)
        t:add(f.port, tvb(off + 3, 1), bits(w0, 6, 4))
        t:add(f.tsn, tvb(off + 3, 3), bits(w0, 3, 0) * 4096 + bits(w1, 31, 20))
        t:add(f.beats, tvb(off + 15, 1), beats)
        t:add(f.size, tvb(off + 15, 1), bits(w3, 2, 0))
        name = "rd"
    elseif ft == 2 then
        if remain < 12 then return nil end
        len = 12
        local t = tree:add(oisa, tvb(off, len), string.format("Write response: ID %d", math.floor(id)))
        t:add(f.ft, tvb(off, 1), ft)
        t:add(f.id, tvb(off, 2), id)
        t:add(f.bresp, tvb(off + 11, 1), bits(tvb(off + 11, 1):uint(), 1, 0))
        name = "b"
    elseif ft == 4 then
        local beats = bits(w0, 18, 17) + 1
        len = 4 + beats * BEAT
        if len > remain then len = remain end
        local t = tree:add(oisa, tvb(off, len), string.format("Read response: ID %d, %d beats", math.floor(id), math.floor(beats)))
        t:add(f.ft, tvb(off, 1), ft)
        t:add(f.id, tvb(off, 2), id)
        t:add(f.beats, tvb(off + 1, 1), beats)
        t:add(f.rresp, tvb(off + 3, 1), bits(w0, 2, 1))
        if len > 4 then t:add(f.data, tvb(off + 4, len - 4)) end
        name = "r"
    else
        return nil
    end
    return len, name, id
end

function oisa.dissector(tvb, pinfo, tree)
    if tvb:len() < 16 or tvb(12, 2):uint() ~= 0x88EE then
        return eth_dissector:call(tvb, pinfo, tree)
    end

    pinfo.cols.protocol = "OISA"
    local h = tree:add(oisa, tvb(0, 16), "OISA 10b header")
    h:add(f.version, tvb(0, 1))
    h:add(f.vc, tvb(0, 1))
    h:add(f.format, tvb(1, 1))
    h:add(f.pkt_type, tvb(1, 1))
    h:add(f.length, tvb(2, 2))
    h:add(f.tag, tvb(6, 2))
    h:add(f.dest, tvb(8, 2))
    h:add(f.pack, tvb(10, 2))
    h:add(f.src, tvb(10, 2))
    h:add(f.etype, tvb(12, 2))
    h:add(f.tail, tvb(14, 2))

    local off, summary = 16, {}
    while off < tvb:len() do
        local len, name, id = dissect_txn(tvb, off, tree)
        if not len then break end
        summary[#summary + 1] = name .. " id=" .. math.floor(id)
        off = off + len
    end
    if off < tvb:len() then tree:add(f.pad, tvb(off)) end

    pinfo.cols.src = "src " .. math.floor(bits(tvb(10, 2):uint(), 9, 0))
    pinfo.cols.dst = "dest " .. math.floor(bits(tvb(8, 2):uint(), 9, 0))
    pinfo.cols.info = string.format("VC=%d type=%d tag=%d pack=%d len=%d: %s",
        bits(tvb(0, 1):uint(), 4, 2), bits(tvb(1, 1):uint(), 4, 0), tvb(6, 2):uint(),
        bits(tvb(10, 2):uint(), 15, 10), bits(tvb(2, 2):uint(), 12, 0), table.concat(summary, ", "))
end

DissectorTable.get("wtap_encap"):add(wtap.ETHERNET, oisa)
