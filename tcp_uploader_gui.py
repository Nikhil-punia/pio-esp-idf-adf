#!/usr/bin/env python3
"""
GUI TCP File Uploader for ESP32
Uploads files to ESP32 raw TCP server using the custom protocol
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
import socket
import struct
import os
import threading
import time
from pathlib import Path

class TCPUploaderGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 TCP File Uploader")
        self.root.geometry("600x500")
        self.root.resizable(True, True)
        
        # Variables
        self.selected_file = tk.StringVar()
        self.esp32_ip = tk.StringVar(value="192.168.1.100")  # Default IP
        self.tcp_port = tk.IntVar(value=8080)
        self.upload_progress = tk.DoubleVar()
        self.upload_status = tk.StringVar(value="Ready")
        self.upload_speed = tk.StringVar(value="")
        self.is_uploading = False
        
        self.setup_ui()
        
    def setup_ui(self):
        # Main frame
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # Configure grid weights
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)
        
        # ESP32 Connection Settings
        settings_frame = ttk.LabelFrame(main_frame, text="ESP32 Connection", padding="5")
        settings_frame.grid(row=0, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 10))
        settings_frame.columnconfigure(1, weight=1)
        
        ttk.Label(settings_frame, text="ESP32 IP:").grid(row=0, column=0, sticky=tk.W, padx=(0, 5))
        ip_entry = ttk.Entry(settings_frame, textvariable=self.esp32_ip, width=15)
        ip_entry.grid(row=0, column=1, sticky=(tk.W, tk.E), padx=(0, 10))
        
        ttk.Label(settings_frame, text="TCP Port:").grid(row=0, column=2, sticky=tk.W, padx=(10, 5))
        port_entry = ttk.Entry(settings_frame, textvariable=self.tcp_port, width=8)
        port_entry.grid(row=0, column=3, sticky=tk.W)
        
        # Test connection button
        test_btn = ttk.Button(settings_frame, text="Test Connection", command=self.test_connection)
        test_btn.grid(row=0, column=4, padx=(10, 0))
        
        # File Selection
        file_frame = ttk.LabelFrame(main_frame, text="File Selection", padding="5")
        file_frame.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 10))
        file_frame.columnconfigure(0, weight=1)
        
        file_entry = ttk.Entry(file_frame, textvariable=self.selected_file, state="readonly")
        file_entry.grid(row=0, column=0, sticky=(tk.W, tk.E), padx=(0, 5))
        
        browse_btn = ttk.Button(file_frame, text="Browse", command=self.browse_file)
        browse_btn.grid(row=0, column=1)
        
        # File info
        self.file_info_label = ttk.Label(file_frame, text="No file selected", foreground="gray")
        self.file_info_label.grid(row=1, column=0, columnspan=2, sticky=tk.W, pady=(5, 0))
        
        # Upload Controls
        upload_frame = ttk.LabelFrame(main_frame, text="Upload", padding="5")
        upload_frame.grid(row=2, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 10))
        upload_frame.columnconfigure(0, weight=1)
        
        # Upload button
        self.upload_btn = ttk.Button(upload_frame, text="Upload File", command=self.start_upload)
        self.upload_btn.grid(row=0, column=0, pady=(0, 10))
        
        # Progress bar
        self.progress_bar = ttk.Progressbar(upload_frame, variable=self.upload_progress, maximum=100)
        self.progress_bar.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=(0, 5))
        
        # Status labels
        status_frame = ttk.Frame(upload_frame)
        status_frame.grid(row=2, column=0, columnspan=2, sticky=(tk.W, tk.E))
        status_frame.columnconfigure(0, weight=1)
        status_frame.columnconfigure(1, weight=1)
        
        self.status_label = ttk.Label(status_frame, textvariable=self.upload_status)
        self.status_label.grid(row=0, column=0, sticky=tk.W)
        
        self.speed_label = ttk.Label(status_frame, textvariable=self.upload_speed)
        self.speed_label.grid(row=0, column=1, sticky=tk.E)
        
        # Log area
        log_frame = ttk.LabelFrame(main_frame, text="Log", padding="5")
        log_frame.grid(row=3, column=0, columnspan=2, sticky=(tk.W, tk.E, tk.N, tk.S), pady=(0, 10))
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        main_frame.rowconfigure(3, weight=1)
        
        self.log_text = scrolledtext.ScrolledText(log_frame, height=8, width=70)
        self.log_text.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # Clear log button
        clear_log_btn = ttk.Button(log_frame, text="Clear Log", command=self.clear_log)
        clear_log_btn.grid(row=1, column=0, pady=(5, 0))
        
        # Protocol info
        info_frame = ttk.LabelFrame(main_frame, text="Protocol Info", padding="5")
        info_frame.grid(row=4, column=0, columnspan=2, sticky=(tk.W, tk.E))
        
        protocol_info = "Protocol: [4 bytes filename_len][4 bytes file_size][filename][file_data] (network byte order)"
        ttk.Label(info_frame, text=protocol_info, font=("Consolas", 8), foreground="blue").grid(row=0, column=0, sticky=tk.W)
        
    def log(self, message):
        """Add message to log with timestamp"""
        timestamp = time.strftime("%H:%M:%S")
        self.log_text.insert(tk.END, f"[{timestamp}] {message}\n")
        self.log_text.see(tk.END)
        self.root.update_idletasks()
        
    def clear_log(self):
        """Clear the log text area"""
        self.log_text.delete(1.0, tk.END)
        
    def browse_file(self):
        """Open file dialog to select a file"""
        filename = filedialog.askopenfilename(
            title="Select file to upload",
            filetypes=[
                ("All files", "*.*"),
                ("Audio files", "*.mp3 *.wav *.flac *.m4a"),
                ("Text files", "*.txt"),
                ("Images", "*.jpg *.jpeg *.png *.gif")
            ]
        )
        
        if filename:
            self.selected_file.set(filename)
            
            # Update file info
            try:
                file_path = Path(filename)
                file_size = file_path.stat().st_size
                size_mb = file_size / (1024 * 1024)
                self.file_info_label.config(
                    text=f"Size: {size_mb:.2f} MB ({file_size:,} bytes)",
                    foreground="black"
                )
                self.log(f"Selected file: {file_path.name} ({size_mb:.2f} MB)")
            except Exception as e:
                self.file_info_label.config(text=f"Error reading file: {e}", foreground="red")
                self.log(f"Error reading file info: {e}")
                
    def test_connection(self):
        """Test connection to ESP32 TCP server"""
        def test():
            try:
                self.log(f"Testing connection to {self.esp32_ip.get()}:{self.tcp_port.get()}...")
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(5)  # 5 second timeout
                
                result = sock.connect_ex((self.esp32_ip.get(), self.tcp_port.get()))
                sock.close()
                
                if result == 0:
                    self.log("✓ Connection successful!")
                    messagebox.showinfo("Connection Test", "Connection to ESP32 successful!")
                else:
                    self.log("✗ Connection failed!")
                    messagebox.showerror("Connection Test", "Could not connect to ESP32. Check IP and port.")
                    
            except Exception as e:
                self.log(f"✗ Connection error: {e}")
                messagebox.showerror("Connection Test", f"Connection error: {e}")
        
        # Run test in background thread
        threading.Thread(target=test, daemon=True).start()
        
    def start_upload(self):
        """Start file upload in background thread"""
        if self.is_uploading:
            messagebox.showwarning("Upload in Progress", "An upload is already in progress!")
            return
            
        if not self.selected_file.get():
            messagebox.showerror("No File Selected", "Please select a file to upload first.")
            return
            
        if not os.path.exists(self.selected_file.get()):
            messagebox.showerror("File Not Found", "Selected file no longer exists.")
            return
            
        # Start upload in background thread
        self.is_uploading = True
        self.upload_btn.config(state="disabled", text="Uploading...")
        threading.Thread(target=self.upload_file, daemon=True).start()
        
    def upload_file(self):
        """Upload file to ESP32 TCP server"""
        try:
            filepath = self.selected_file.get()
            filename = os.path.basename(filepath)
            
            self.log(f"Starting upload: {filename}")
            self.upload_status.set("Connecting...")
            self.upload_progress.set(0)
            
            # Get file info
            file_size = os.path.getsize(filepath)
            filename_bytes = filename.encode('utf-8')
            filename_len = len(filename_bytes)
            
            self.log(f"File size: {file_size:,} bytes")
            self.log(f"Filename length: {filename_len} bytes")
            
            # Connect to ESP32
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(30)  # 30 second timeout
            
            self.log(f"Connecting to {self.esp32_ip.get()}:{self.tcp_port.get()}...")
            sock.connect((self.esp32_ip.get(), self.tcp_port.get()))
            self.log("✓ Connected to ESP32")
            
            # Send header (network byte order)
            header = struct.pack('!II', filename_len, file_size)  # ! = network byte order
            sock.send(header)
            self.log(f"Sent header: filename_len={filename_len}, file_size={file_size}")
            
            # Send filename
            sock.send(filename_bytes)
            self.log(f"Sent filename: {filename}")
            
            # Send file data with progress updates
            self.upload_status.set("Uploading...")
            start_time = time.time()
            bytes_sent = 0
            
            with open(filepath, 'rb') as f:
                while True:
                    chunk = f.read(4096)  # 4KB chunks
                    if not chunk:
                        break
                        
                    sock.send(chunk)
                    bytes_sent += len(chunk)
                    
                    # Update progress
                    progress = (bytes_sent / file_size) * 100
                    self.upload_progress.set(progress)
                    
                    # Calculate speed
                    elapsed = time.time() - start_time
                    if elapsed > 0:
                        speed_bps = bytes_sent / elapsed
                        speed_kbps = speed_bps / 1024
                        self.upload_speed.set(f"{speed_kbps:.1f} KB/s")
                    
                    # Update status
                    self.upload_status.set(f"Uploaded {bytes_sent:,} / {file_size:,} bytes")
                    
            # Wait for response
            self.upload_status.set("Waiting for response...")
            response = sock.recv(10).decode('utf-8', errors='ignore')
            sock.close()
            
            # Final status
            elapsed = time.time() - start_time
            speed_kbps = (file_size / 1024) / elapsed if elapsed > 0 else 0
            
            if response.startswith('OK'):
                self.upload_progress.set(100)
                self.upload_status.set("Upload completed!")
                self.upload_speed.set(f"Avg: {speed_kbps:.1f} KB/s")
                self.log(f"✓ Upload successful! ({elapsed:.1f}s, {speed_kbps:.1f} KB/s)")
                messagebox.showinfo("Upload Complete", f"File uploaded successfully!\nTime: {elapsed:.1f}s\nSpeed: {speed_kbps:.1f} KB/s")
            else:
                self.log(f"✗ Upload failed. ESP32 response: {response}")
                messagebox.showerror("Upload Failed", f"ESP32 reported an error: {response}")
                
        except socket.timeout:
            self.log("✗ Upload failed: Connection timeout")
            messagebox.showerror("Upload Failed", "Connection timeout. Check ESP32 connection.")
            
        except ConnectionRefusedError:
            self.log("✗ Upload failed: Connection refused")
            messagebox.showerror("Upload Failed", "Connection refused. Check if ESP32 TCP server is running.")
            
        except Exception as e:
            self.log(f"✗ Upload error: {e}")
            messagebox.showerror("Upload Error", f"Upload failed: {e}")
            
        finally:
            # Reset UI
            self.is_uploading = False
            self.upload_btn.config(state="normal", text="Upload File")
            if not self.upload_status.get().endswith("completed!"):
                self.upload_status.set("Ready")
                self.upload_speed.set("")
                self.upload_progress.set(0)

def main():
    root = tk.Tk()
    app = TCPUploaderGUI(root)
    
    # Center window on screen
    root.update_idletasks()
    x = (root.winfo_screenwidth() // 2) - (root.winfo_width() // 2)
    y = (root.winfo_screenheight() // 2) - (root.winfo_height() // 2)
    root.geometry(f"+{x}+{y}")
    
    try:
        root.mainloop()
    except KeyboardInterrupt:
        print("\nShutting down...")

if __name__ == "__main__":
    main()
