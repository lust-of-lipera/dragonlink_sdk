import subprocess
import pyudev

def find_dragonlink_usb():
    context = pyudev.Context()
    devices = context.list_devices(subsystem='usb', DEVTYPE='usb_device')
    
    for device in devices:
        bus = int(device.attributes.asstring('busnum'))
        device_num = int(device.attributes.asstring('devnum'))
        vendor_id = device.attributes.asstring('idVendor')
        product_id = device.attributes.asstring('idProduct')
        manufacturer = device.attributes.get('manufacturer', 'Unknown')
        product = device.attributes.get('product', 'Unknown')
        print(f'USB: {bus:03d} Device {device_num:03d}: ID {vendor_id}:{product_id} {manufacturer} {product}')
        
        if manufacturer == b"NXP" and product == b"VCOM Port":
            return  f"{vendor_id}:{product_id}"

    raise Exception("Did not find dragon link usb")

def create_user_id_packet(input_filename, output_filename, find_str, hex_str):
    # Read the entire content of the input file
    with open(input_filename, 'r') as input_file:
        file_content = input_file.read()

    # Ensure hex_str is exactly 4 characters long
    if len(hex_str) < 4:
        hex_str_padded = hex_str.rjust(4, '0')  # Pad with ASCII '0'
    else:
        hex_str_padded = hex_str[:4]  # Take only the first 4 characters

    # Replace find_str with hex_str_padded
    modified_content = file_content.replace(find_str, hex_str_padded)

    # Write the modified content to the output file
    with open(output_filename, 'w') as output_file:
        output_file.write(modified_content)

# dl is dragon link
def int_to_dl_packet_format(n: int):
    if n > 999 or n < 0:
        raise ValueError("The max value supported by my shitty math is 999")
    if n <= 255:
        return hex(n)[2:]

    n = str(n)
    # example 123
    # upper is 1 
    upper = n[:1]

    # lower is 23 
    lower = n[1:]

    int_lower = int(lower) 
    if int_lower < 10:
        return upper + lower

    if int_lower <= 0xf:
        return upper + '0' + hex(int(lower))[2:]

    return upper + hex(int(lower))[2:]

    # sudo ./usb_replay 1fc9:0083 1.bin

def inject_to_dragon_link(dl_usb_hex: str, packet_path):
    command = ["sudo", "./usb_replay", dl_usb_hex, packet_path]
    return subprocess.run(command, capture_output=True, text=True)

# ID > 0 && ID < 999
def change_id_dragonlink(in_id: int):
    dl_usb_hex = find_dragonlink_usb() 
    print(f"{dl_usb_hex=}")
    
    byte2replace = int_to_dl_packet_format(in_id)   
    print(f"{byte2replace=}")

    output_packet = 'output.txt'
    # The four-character string to find and replace from base.bin
    find_str = 'ffff'
    # this creates output.bin
    create_user_id_packet('base.bin', output_packet, find_str, byte2replace)

    res = inject_to_dragon_link(dl_usb_hex, output_packet)
    print("STDOUT:", res.stdout)
    print("STDERR:", res.stderr)
    print("Return Code:", res.returncode)

    if res.returncode == 0:
        print(f"Worked! Injected ID = [{in_id}] to {dl_usb_hex} dragonlink")
    else:
        print(f"Failed err code == {res.returncode}\nUSB driver replay requires root did you run with sudo?")

def main():
    change_id_dragonlink(707)
    
if __name__ == "__main__":
    main()