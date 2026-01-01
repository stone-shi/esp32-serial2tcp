#!/usr/bin/env python3
import sys
import time
import socket
import serial
import os
import argparse
import threading
import random

# Check for common issue where 'serial' package is installed instead of 'pyserial'
if not hasattr(serial, 'Serial'):
    print("\n[ERROR] The installed 'serial' module is incorrect.")
    print(f"Module found at: {serial.__file__}")
    print("It seems you have installed the package 'serial' instead of 'pyserial'.")
    print("Please fix your environment by running:")
    print("  pip3 uninstall -y serial && pip3 install pyserial")
    sys.exit(1)

class BridgeTester:
    def __init__(self, serial_port, baudrate, tcp_ip, tcp_port, timeout=5):
        self.serial_port = serial_port
        self.baudrate = baudrate
        self.tcp_ip = tcp_ip
        self.tcp_port = tcp_port
        self.timeout = timeout
        self.ser = None
        self.sock = None

    def connect(self):
        print(f"[INFO] Opening Serial {self.serial_port} @ {self.baudrate}...")
        self.ser = serial.Serial(self.serial_port, self.baudrate, timeout=self.timeout)
        
        print(f"[INFO] Connecting to TCP {self.tcp_ip}:{self.tcp_port}...")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect((self.tcp_ip, self.tcp_port))
        print("[INFO] Connected.")

    def close(self):
        if self.ser: 
            self.ser.close()
        if self.sock: 
            self.sock.close()

    def _read_n_bytes_from_socket(self, n):
        data = b''
        while len(data) < n:
            try:
                chunk = self.sock.recv(min(4096, n - len(data)))
                if not chunk:
                    break
                data += chunk
            except socket.timeout:
                break
        return data

    def _read_n_bytes_from_serial(self, n):
        data = b''
        while len(data) < n:
            chunk = self.ser.read(min(4096, n - len(data)))
            if not chunk: # Timeout
                break
            data += chunk
        return data

    def test_serial_to_tcp(self, data_size):
        """Writes to Serial, expects data on TCP"""
        print(f"  -> Testing Serial -> TCP ({data_size} bytes)... ", end='', flush=True)
        payload = os.urandom(data_size)
        
        received_data = b''
        def read_thread_func():
            nonlocal received_data
            received_data = self._read_n_bytes_from_socket(data_size)

        # Start reading from TCP before writing to Serial to avoid buffer deadlocks
        t = threading.Thread(target=read_thread_func)
        t.start()

        # Write to serial
        self.ser.write(payload)
        self.ser.flush()
        
        t.join()

        if payload == received_data:
            print("PASS")
            return True
        else:
            print(f"FAIL (Sent {len(payload)}, Recv {len(received_data)})")
            return False

    def test_tcp_to_serial(self, data_size):
        """Writes to TCP, expects data on Serial"""
        print(f"  <- Testing TCP -> Serial ({data_size} bytes)... ", end='', flush=True)
        payload = os.urandom(data_size)

        received_data = b''
        def read_thread_func():
            nonlocal received_data
            received_data = self._read_n_bytes_from_serial(data_size)

        # Start reading from Serial before writing to TCP
        t = threading.Thread(target=read_thread_func)
        t.start()

        # Write to TCP
        try:
            self.sock.sendall(payload)
        except Exception as e:
            print(f"Socket send error: {e}")

        t.join()

        if payload == received_data:
            print("PASS")
            return True
        else:
            print(f"FAIL (Sent {len(payload)}, Recv {len(received_data)})")
            return False

def main():
    parser = argparse.ArgumentParser(description='ESP32 Serial-TCP Bridge Tester')
    parser.add_argument('--port', required=True, help='Serial port (e.g. /dev/ttyUSB0)')
    parser.add_argument('--baud', type=int, default=9600, help='Baud rate (default: 9600)')
    parser.add_argument('--ip', required=True, help='ESP32 IP Address')
    parser.add_argument('--tcp-port', type=int, default=8989, help='TCP Port (default: 8989)')
    parser.add_argument('--size', type=int, default=1024, help='Data size per iteration (bytes)')
    parser.add_argument('--iterations', type=int, default=1, help='Number of iterations (functional test)')
    parser.add_argument('--stress', action='store_true', help='Run stress test (infinite loop)')
    
    args = parser.parse_args()
    tester = BridgeTester(args.port, args.baud, args.ip, args.tcp_port)
    
    try:
        tester.connect()
        count = 0
        while True:
            count += 1
            print(f"\n--- Iteration {count} ---")
            if not tester.test_serial_to_tcp(args.size): break
            if not tester.test_tcp_to_serial(args.size): break
            if not args.stress and count >= args.iterations: break
            time.sleep(0.1)
    except KeyboardInterrupt: print("\n[INFO] Interrupted by user")
    except Exception as e: print(f"\n[ERROR] {e}")
    finally: tester.close()

if __name__ == "__main__":
    main()