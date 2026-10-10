-- Wireshark Lua dissector for talk2xumeng/axi-test (eth and experimental sue).
-- Install in Wireshark's personal plugins folder, then restart Wireshark.
-- This is a postdissector: it reads the whole Ethernet frame and adds an AXI
-- tree even when Wireshark routes raw 802.3 frames to its LLC dissector.

local bits = bit32 or bit
local rshift = bits and bits.rshift or function(x, n) return math.floor(x / 2^n) end
local band = bits and bits.band or function(a, b)
    local result, power = 0, 1
    while a > 0 and b > 0 do
        if a % 2 == 1 and b % 2 == 1 then result = result + power end
        a, b, power = math.floor(a / 2), math.floor(b / 2), power * 2
    end
    return result
end

local axi = Proto("axiperf", "AXI over Ethernet (axiperf)")
axi.prefs.eth = Pref.bool("Decode raw 802.3 Length format", true,
    "Decode VLAN-tagged raw 802.3 AXI frames (no LLC header)")
axi.prefs.sue = Pref.bool("Decode experimental SUE format", true,
    "Decode VLAN-tagged SUE frames by EtherType and PackNum")
axi.prefs.sue_type = Pref.uint("SUE EtherType (decimal)", 0x88B5,
    "Default 0x88B5 = 34997; match --sue-ethertype", 10)

local f = {
    format = ProtoField.string("axiperf.format", "Encapsulation"),
    dest = ProtoField.bytes("axiperf.dst", "Destination (MAC / GPU ID)"),
    source = ProtoField.bytes("axiperf.src", "Source (MAC / GPU ID)"),
    dst_gpu = ProtoField.uint16("axiperf.dst_gpu", "Destination GPU ID", base.HEX),
    src_gpu = ProtoField.uint16("axiperf.src_gpu", "Source GPU ID", base.HEX),
    pcp = ProtoField.uint8("axiperf.pcp", "PCP / VC", base.DEC),
    dei = ProtoField.uint8("axiperf.dei", "DEI / OCFI", base.DEC),
    vid = ProtoField.uint16("axiperf.vid", "VLAN ID", base.DEC),
    length = ProtoField.uint16("axiperf.length", "AXI payload length", base.DEC),
    etype = ProtoField.uint16("axiperf.etype", "SUE EtherType", base.HEX),
    pack = ProtoField.uint8("axiperf.pack", "PackNum", base.DEC),
    sue_format = ProtoField.uint8("axiperf.sue_format", "SUE Format", base.DEC),
    pkt_type = ProtoField.uint8("axiperf.pkt_type", "SUE PktType", base.DEC),
    transaction = ProtoField.string("axiperf.transaction", "Transaction"),
    frame_type = ProtoField.uint8("axiperf.frame_type", "Frame type", base.HEX),
    id = ProtoField.uint16("axiperf.id", "AXI ID", base.DEC),
    awlen = ProtoField.uint8("axiperf.awlen", "AWLEN (encoded beats minus 1)", base.DEC),
    wfull = ProtoField.uint8("axiperf.wfull", "writeFull", base.HEX),
    awuser = ProtoField.string("axiperf.awuser", "AWUSER [56:0]"),
    awuser_hi = ProtoField.uint16("axiperf.awuser_56_44", "AWUSER [56:44]", base.HEX),
    awuser_mid = ProtoField.uint32("axiperf.awuser_43_12", "AWUSER [43:12]", base.HEX),
    awuser_lo = ProtoField.uint16("axiperf.awuser_11_0", "AWUSER [11:0]", base.HEX),
    awaddr = ProtoField.string("axiperf.awaddr", "AWADDR [48:0]"),
    awaddr_hi = ProtoField.uint32("axiperf.awaddr_48_29", "AWADDR [48:29]", base.HEX),
    awaddr_lo = ProtoField.uint32("axiperf.awaddr_28_0", "AWADDR [28:0]", base.HEX),
    beats = ProtoField.uint8("axiperf.beats", "Beats", base.DEC),
    size = ProtoField.uint8("axiperf.size", "AXI size (log2 bytes/beat)", base.DEC),
    last = ProtoField.bool("axiperf.rlast", "RLAST"),
    response = ProtoField.uint8("axiperf.response", "Response", base.DEC),
    data = ProtoField.bytes("axiperf.data", "Data"),
    strb = ProtoField.bytes("axiperf.wstrb", "WSTRB"),
    padding = ProtoField.bytes("axiperf.padding", "Ethernet padding / trailer"),
    problem = ProtoField.string("axiperf.problem", "Decode warning")
}
axi.fields = f

local names = { [0] = "WriteFull", [1] = "Write", [2] = "B response",
                [3] = "Read request", [4] = "R response" }
local channels = { [0] = 0, [1] = 0, [2] = 2, [3] = 1, [4] = 3 }

local function u16(t, offset)
    return t(offset, 2):uint()
end

local function u32(t, offset)
    return t(offset, 4):uint()
end

-- Return length, beats, size, data offset, data length, and optional error.
local function txn_shape(t, off, limit, ft, w0)
    local beats = band(rshift(w0, 17), 3) + 1
    local size, step, dataoff, datalen
    if ft == 0 or ft == 1 then
        if off + 16 > limit then return nil, nil, nil, nil, nil, "Incomplete write header" end
        size = band(u32(t, off + 12), 7)
        step = 16 + beats * (ft == 0 and 64 or 72)
        dataoff, datalen = off + 16, beats * 64
    elseif ft == 3 then
        if off + 16 > limit then return nil, nil, nil, nil, nil, "Incomplete read header" end
        local w3 = u32(t, off + 12)
        beats = band(rshift(w3, 3), 3) + 1
        size = band(w3, 7)
        step = 16
    elseif ft == 2 then
        step = 12
    elseif ft == 4 then
        step = 4 + beats * 64
        dataoff, datalen = off + 4, beats * 64
    else
        return nil, nil, nil, nil, nil, "Unknown frame_type " .. ft
    end
    if off + step > limit then
        return nil, nil, nil, nil, nil, "Transaction extends past AXI payload"
    end
    return step, beats, size, dataoff, datalen
end

function axi.dissector(tvb, pinfo, tree)
    local cap = tvb:len()
    if cap < 22 or u16(tvb, 12) ~= 0x8100 then return end

    local field16 = u16(tvb, 16)
    local is_eth = field16 <= 1500 and axi.prefs.eth
    local is_sue = field16 == axi.prefs.sue_type and axi.prefs.sue
    if not is_eth and not is_sue then return end

    local hl = is_eth and 18 or 20
    if cap < hl + 4 then return end
    local payload_len, count
    if is_eth then
        payload_len = field16
        if payload_len < 4 or hl + payload_len > cap then return end
    else
        count = band(rshift(u16(tvb, 18), 8), 0x3f)
        if count < 1 or count > 16 then return end
        payload_len = cap - hl
    end

    local first = rshift(u32(tvb, hl), 28)
    if not names[first] then return end
    -- Only decode plausible AXI traffic; PCP mismatch is displayed, not rejected.
    local endpos = hl + payload_len
    local offset, parsed, warning = hl, 0, nil
    local records = {}
    while offset < endpos and parsed < 16 and (not count or parsed < count) do
        if offset + 4 > endpos then warning = "Incomplete AXI word"; break end
        local w0 = u32(tvb, offset)
        local ft = rshift(w0, 28)
        local step, beats, size, dataoff, datalen, err = txn_shape(tvb, offset, endpos, ft, w0)
        if not step then warning = err; break end
        records[#records + 1] = { off=offset, len=step, ft=ft,
            id=band(rshift(w0, 19), 0x1ff), beats=beats, size=size,
            dataoff=dataoff, datalen=datalen, w0=w0 }
        offset = offset + step
        parsed = parsed + 1
    end
    if is_eth and offset ~= endpos and not warning then
        warning = "Payload has extra bytes or more than 16 transactions"
    end
    if is_sue and parsed ~= count and not warning then
        warning = "Fewer transactions than PackNum"
    end
    -- Avoid claiming unrelated VLAN traffic solely because its Length is short.
    if parsed == 0 then return end

    local root = tree:add(axi, tvb(0, cap), string.format(
        "AXI over Ethernet (%s, %d transaction%s%s)",
        is_eth and "eth" or "sue", parsed, parsed == 1 and "" or "s",
        warning and ", malformed" or ""))
    root:add(f.format, is_eth and "eth / 802.3 Length" or "sue / EtherType + PackNum")
    root:add(f.dest, tvb(0, 6))
    root:add(f.source, tvb(6, 6))
    if is_sue then
        root:add(f.dst_gpu, tvb(0, 2))
        root:add(f.src_gpu, tvb(6, 2))
    end
    local tci = u16(tvb, 14)
    local pcp = band(rshift(tci, 13), 7)
    root:add(f.pcp, pcp)
    root:add(f.dei, band(rshift(tci, 12), 1))
    root:add(f.vid, band(tci, 0xfff))
    if is_eth then
        root:add(f.length, tvb(16, 2), payload_len)
    else
        local ext = u16(tvb, 18)
        root:add(f.etype, tvb(16, 2))
        root:add(f.pack, count)
        root:add(f.sue_format, band(rshift(ext, 5), 7))
        root:add(f.pkt_type, band(ext, 31))
    end

    for i, r in ipairs(records) do
        local label = string.format("%d: %s, ID %d", i, names[r.ft], r.id)
        local node = root:add(f.transaction, tvb(r.off, r.len), label)
        node:add(f.frame_type, r.ft)
        node:add(f.id, r.id)
        if r.ft == 0 or r.ft == 1 then
            -- W0: type[31:28], awid[27:19], awlen[18:17],
            --     writeFull[16:13], awuser[56:44][12:0]
            -- W1: awuser[43:12]
            -- W2: awuser[11:0][31:20], awaddr[48:29][19:0]
            -- W3: awaddr[28:0][31:3], awsize[2:0]
            local w1 = u32(tvb, r.off + 4)
            local w2 = u32(tvb, r.off + 8)
            local w3 = u32(tvb, r.off + 12)
            local user_hi = band(r.w0, 0x1fff)
            local user_lo = rshift(w2, 20)
            -- Split into two integer chunks so no 57-bit value is rounded
            -- by Lua's floating point number representation.
            local user_top25 = user_hi * 4096 + rshift(w1, 20)
            local user_bottom32 = band(w1, 0xfffff) * 4096 + user_lo
            local addr_hi = band(w2, 0xfffff)
            local addr_lo = rshift(w3, 3)
            node:add(f.awlen, band(rshift(r.w0, 17), 3))
            node:add(f.wfull, band(rshift(r.w0, 13), 15))
            node:add(f.awuser, string.format("0x%07X%08X", user_top25, user_bottom32))
            node:add(f.awuser_hi, user_hi)
            node:add(f.awuser_mid, w1)
            node:add(f.awuser_lo, user_lo)
            local addr_top17 = rshift(addr_hi, 3)
            local addr_bottom32 = band(addr_hi, 7) * 536870912 + addr_lo
            node:add(f.awaddr, string.format("0x%05X%08X",
                addr_top17, addr_bottom32))
            node:add(f.awaddr_hi, addr_hi)
            node:add(f.awaddr_lo, addr_lo)
        end
        if r.ft == 0 or r.ft == 1 or r.ft == 3 or r.ft == 4 then
            node:add(f.beats, r.beats)
        end
        if r.size then node:add(f.size, r.size) end
        if r.ft == 4 then
            node:add(f.last, band(rshift(r.w0, 3), 1) ~= 0)
            node:add(f.response, band(rshift(r.w0, 4), 3))
        elseif r.ft == 2 then
            node:add(f.response, band(rshift(r.w0, 4), 3))
        end
        if r.dataoff then
            if r.ft == 1 then
                for b = 0, r.beats - 1 do
                    local start = r.dataoff + b * 72
                    node:add(f.strb, tvb(start, 8))
                    node:add(f.data, tvb(start + 8, 64))
                end
            else
                node:add(f.data, tvb(r.dataoff, r.datalen))
            end
        end
        if channels[r.ft] ~= pcp then
            node:add(f.problem, string.format("PCP %d differs from expected VC %d", pcp, channels[r.ft]))
        end
    end
    if warning then root:add(f.problem, warning) end
    if offset < cap then root:add(f.padding, tvb(offset, cap - offset)) end
    pinfo.cols.protocol = "AXI"
    pinfo.cols.info:append(string.format(" [AXI %s %s, %d txn%s%s]",
        is_eth and "eth" or "sue", names[first], parsed,
        parsed == 1 and "" or "s", warning and ", malformed" or ""))
end

register_postdissector(axi)
