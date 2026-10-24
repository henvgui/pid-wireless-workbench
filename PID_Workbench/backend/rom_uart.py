"""Legacy ROM-protocol reference, retained for regression tests only.

Not connected to the product API or task engine. Hardware access in this
workbench always goes through host USB -> ESP-NOW -> slave target interface.
"""
import time
import serial


class RomError(RuntimeError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


class Rom:
    def __init__(self, port):
        self.port = port

    def read(self, size, timeout=2):
        result = bytearray()
        deadline = time.monotonic() + timeout
        while len(result) < size and time.monotonic() < deadline:
            result.extend(self.port.read(size - len(result)))
        if len(result) != size:
            raise RomError("uart_timeout", "STM32 ROM 超时；检查所选串口、BOOT0/BOOT1、复位和交叉 TX/RX。")
        return bytes(result)

    def ack(self, timeout=2):
        value = self.read(1, timeout)[0]
        if value != 0x79:
            raise RomError("uart_nack", f"STM32 ROM 返回 {value:#04x}，没有确认此操作；不会自动重写。")

    def command(self, command):
        self.port.write(bytes([command, command ^ 255]))
        self.ack()

    def address(self, address):
        data = address.to_bytes(4, "big")
        self.port.write(data + bytes([data[0] ^ data[1] ^ data[2] ^ data[3]]))
        self.ack()

    def memory(self, address, size):
        self.command(0x11)
        self.address(address)
        self.port.write(bytes([size - 1, (size - 1) ^ 255]))
        self.ack()
        return self.read(size)


def program(image, chip, port_name, run, emit, serial_factory=serial.Serial):
    f103 = chip["profile"] == "f103c8"
    expected_id = 0x410 if f103 else 0x468
    page_size, alignment = (1024, 4) if f103 else (2048, 8)
    capacity_address = 0x1FFFF7E0 if f103 else 0x1FFF75E0
    result = {"verified": False, "go_ack": False, "transport": "serial_rom"}
    port = serial_factory(port=None, baudrate=115200, bytesize=8, parity=serial.PARITY_EVEN,
                          stopbits=1, timeout=.05, write_timeout=2)
    port.dtr, port.rts, port.port = False, False, port_name
    try:
        port.open()
        port.reset_input_buffer()
        rom = Rom(port)
        emit("probe", .05, "等待 STM32 系统 ROM；115200 / 8E1，BOOT/RESET 由目标板手动设置。")
        port.write(b"\x7f")
        rom.ack()
        rom.command(0)
        count = rom.read(1)[0]
        if not 1 <= count <= 64:
            raise RomError("unsupported_rom", "ROM 指令列表长度无效。")
        version = rom.read(1)[0]
        commands = set(rom.read(count))
        rom.ack()
        erase = 0x43 if f103 and 0x43 in commands else 0x44 if 0x44 in commands else None
        if not {2, 0x11, 0x31}.issubset(commands) or erase is None or run and 0x21 not in commands:
            raise RomError("unsupported_rom", "目标 ROM 缺少读取、写入、擦除或本次所需的启动命令。")
        rom.command(2)
        if rom.read(1) != b"\x01":
            raise RomError("wrong_device", "设备标识长度无效。")
        device_id = int.from_bytes(rom.read(2), "big")
        rom.ack()
        if device_id != expected_id:
            raise RomError("wrong_device", f"所选芯片期望 ID {expected_id:#x}，实测 {device_id:#x}；擦除前终止。")
        actual_kb = int.from_bytes(rom.memory(capacity_address, 2), "little")
        if actual_kb != chip["flash_kb"] and not (f103 and actual_kb == 128):
            raise RomError("flash_capacity", f"容量回报 {actual_kb} KiB，与选择的 {chip['flash_kb']} KiB 配置不兼容。")
        result.update(device_id=device_id, actual_flash_kb=actual_kb, rom_version=version)
        emit("erase", .12, f"ID {device_id:#x}，容量回报 {actual_kb} KiB；只擦除镜像覆盖的页。")
        pages = (len(image) + page_size - 1) // page_size
        rom.command(erase)
        data = bytearray((pages - 1).to_bytes(2 if erase == 0x44 else 1, "big"))
        for page in range(pages):
            data.extend(page.to_bytes(2 if erase == 0x44 else 1, "big"))
        checksum = 0
        for value in data:
            checksum ^= value
        port.write(data + bytes([checksum]))
        rom.ack(30)
        for offset in range(0, len(image), 256):
            block = image[offset:offset+256]
            padded = block + b"\xff" * (-len(block) % alignment)
            rom.command(0x31)
            rom.address(0x08000000 + offset)
            data = bytes([len(padded)-1]) + padded
            checksum = 0
            for value in data:
                checksum ^= value
            port.write(data + bytes([checksum]))
            rom.ack(5)  # Ambiguous write ACK fails; never replay a Flash write.
            emit("write", .15 + .5*(offset+len(block))/len(image), f"写入 {offset+len(block)}/{len(image)} 字节")
        for offset in range(0, len(image), 256):
            block = image[offset:offset+256]
            if rom.memory(0x08000000+offset, len(block)) != block:
                raise RomError("readback_mismatch", f"完整读回在偏移 {offset} 处不一致，停止启动。")
            emit("verify", .65 + .3*(offset+len(block))/len(image), f"读回 {offset+len(block)}/{len(image)} 字节")
        result["verified"] = True
        if run:
            emit("start", .98, "完整读回一致，发送 ROM GO。")
            rom.command(0x21)
            rom.address(0x08000000)
            result["go_ack"] = True
        return result
    finally:
        port.close()
