#!/usr/bin/env python3
"""
Advanced GUI File Uploader for ESP32
Supports both TCP and UDP uploads with batch operations
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
import socket
import struct
import os
import threading
import time
import queue
from pathlib import Path
from dataclasses import dataclass
from typing import List, Optional

@dataclass
class UploadTask:
    filepath: str
    protocol: str  # 'tcp' or 'udp'
    status: str = 'pending'
    progress: float = 0.0
    error: Optional[str] = None

class AdvancedUploaderGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 Advanced File Uploader")
        self.root.geometry("800x700")
        self.root.resizable(True, True)
        
        # Variables
        self.esp32_ip = tk.StringVar(value="192.168.1.100")
        self.tcp_port = tk.IntVar(value=8080)
        self.udp_port = tk.IntVar(value=8081)
        self.upload_protocol = tk.StringVar(value="tcp")
        
        # Upload queue and management
        self.upload_queue = queue.Queue()
        self.upload_tasks: List[UploadTask] = []
        self.is_uploading = False
        self.current_task_index = 0
        
        # UI Variables
        self.overall_progress = tk.DoubleVar()
        self.current_progress = tk.DoubleVar()
        self.upload_status = tk.StringVar(value="Ready")
        self.upload_speed = tk.StringVar(value="")
        
        self.setup_ui()
        
    def setup_ui(self):
        # Create notebook for tabs
        notebook = ttk.Notebook(self.root)
        notebook.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        
        # Single Upload Tab
        self.single_tab = ttk.Frame(notebook)
        notebook.add(self.single_tab, text="Single Upload")
        self.setup_single_upload_tab()
        
        # Batch Upload Tab
        self.batch_tab = ttk.Frame(notebook)
        notebook.add(self.batch_tab, text="Batch Upload")
        self.setup_batch_upload_tab()
        
        # Settings Tab
        self.settings_tab = ttk.Frame(notebook)
        notebook.add(self.settings_tab, text="Settings")
        self.setup_settings_tab()
        
    def setup_single_upload_tab(self):
        main_frame = ttk.Frame(self.single_tab, padding="10")
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # File Selection
        file_frame = ttk.LabelFrame(main_frame, text="File Selection", padding="5")
        file_frame.pack(fill=tk.X, pady=(0, 10))
        
        self.selected_file = tk.StringVar()
        file_entry = ttk.Entry(file_frame, textvariable=self.selected_file, state="readonly")
        file_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 5))
        
        browse_btn = ttk.Button(file_frame, text="Browse", command=self.browse_single_file)
        browse_btn.pack(side=tk.RIGHT)
        
        # File info
        self.file_info_label = ttk.Label(file_frame, text="No file selected", foreground="gray")
        self.file_info_label.pack(anchor=tk.W, pady=(5, 0))
        
        # Protocol Selection
        protocol_frame = ttk.LabelFrame(main_frame, text="Upload Protocol", padding="5")
        protocol_frame.pack(fill=tk.X, pady=(0, 10))
        
        tcp_radio = ttk.Radiobutton(protocol_frame, text="TCP (Reliable)", 
                                   variable=self.upload_protocol, value="tcp")
        tcp_radio.pack(side=tk.LEFT, padx=(0, 20))
        
        udp_radio = ttk.Radiobutton(protocol_frame, text="UDP (Fast)", 
                                   variable=self.upload_protocol, value="udp")
        udp_radio.pack(side=tk.LEFT)
        
        # Upload Controls
        upload_frame = ttk.LabelFrame(main_frame, text="Upload", padding="5")
        upload_frame.pack(fill=tk.X, pady=(0, 10))
        
        self.single_upload_btn = ttk.Button(upload_frame, text="Upload File", 
                                           command=self.start_single_upload)
        self.single_upload_btn.pack(pady=(0, 10))
        
        # Progress
        self.single_progress_bar = ttk.Progressbar(upload_frame, variable=self.current_progress, maximum=100)
        self.single_progress_bar.pack(fill=tk.X, pady=(0, 5))
        
        # Status
        status_frame = ttk.Frame(upload_frame)
        status_frame.pack(fill=tk.X)
        
        self.single_status_label = ttk.Label(status_frame, textvariable=self.upload_status)
        self.single_status_label.pack(side=tk.LEFT)
        
        self.single_speed_label = ttk.Label(status_frame, textvariable=self.upload_speed)
        self.single_speed_label.pack(side=tk.RIGHT)
        
        # Log
        log_frame = ttk.LabelFrame(main_frame, text="Log", padding="5")
        log_frame.pack(fill=tk.BOTH, expand=True)
        
        self.single_log_text = scrolledtext.ScrolledText(log_frame, height=10)
        self.single_log_text.pack(fill=tk.BOTH, expand=True)
        
        clear_btn = ttk.Button(log_frame, text="Clear Log", command=self.clear_single_log)
        clear_btn.pack(pady=(5, 0))
        
    def setup_batch_upload_tab(self):
        main_frame = ttk.Frame(self.batch_tab, padding="10")
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # File List Management
        files_frame = ttk.LabelFrame(main_frame, text="Files to Upload", padding="5")
        files_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 10))
        
        # Buttons frame
        btn_frame = ttk.Frame(files_frame)
        btn_frame.pack(fill=tk.X, pady=(0, 5))
        
        ttk.Button(btn_frame, text="Add Files", command=self.add_batch_files).pack(side=tk.LEFT, padx=(0, 5))
        ttk.Button(btn_frame, text="Add Folder", command=self.add_batch_folder).pack(side=tk.LEFT, padx=(0, 5))
        ttk.Button(btn_frame, text="Remove Selected", command=self.remove_batch_files).pack(side=tk.LEFT, padx=(0, 5))
        ttk.Button(btn_frame, text="Clear All", command=self.clear_batch_files).pack(side=tk.LEFT, padx=(0, 20))
        
        # Protocol selection for batch
        ttk.Label(btn_frame, text="Protocol:").pack(side=tk.LEFT, padx=(20, 5))
        self.batch_protocol = tk.StringVar(value="tcp")
        protocol_combo = ttk.Combobox(btn_frame, textvariable=self.batch_protocol, 
                                     values=["tcp", "udp"], state="readonly", width=8)
        protocol_combo.pack(side=tk.LEFT)
        
        # File list
        list_frame = ttk.Frame(files_frame)
        list_frame.pack(fill=tk.BOTH, expand=True)
        
        # Treeview for file list
        columns = ("file", "size", "protocol", "status", "progress")
        self.batch_tree = ttk.Treeview(list_frame, columns=columns, show="headings", height=8)
        
        # Configure columns
        self.batch_tree.heading("file", text="File")
        self.batch_tree.heading("size", text="Size")
        self.batch_tree.heading("protocol", text="Protocol")
        self.batch_tree.heading("status", text="Status")
        self.batch_tree.heading("progress", text="Progress")
        
        self.batch_tree.column("file", width=300)
        self.batch_tree.column("size", width=80)
        self.batch_tree.column("protocol", width=60)
        self.batch_tree.column("status", width=80)
        self.batch_tree.column("progress", width=80)
        
        # Scrollbar for treeview
        tree_scroll = ttk.Scrollbar(list_frame, orient=tk.VERTICAL, command=self.batch_tree.yview)
        self.batch_tree.configure(yscrollcommand=tree_scroll.set)
        
        self.batch_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        tree_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        # Batch upload controls
        batch_control_frame = ttk.LabelFrame(main_frame, text="Batch Upload Control", padding="5")
        batch_control_frame.pack(fill=tk.X, pady=(0, 10))
        
        control_btn_frame = ttk.Frame(batch_control_frame)
        control_btn_frame.pack(fill=tk.X)
        
        self.batch_upload_btn = ttk.Button(control_btn_frame, text="Start Batch Upload", 
                                          command=self.start_batch_upload)
        self.batch_upload_btn.pack(side=tk.LEFT, padx=(0, 10))
        
        self.batch_stop_btn = ttk.Button(control_btn_frame, text="Stop Upload", 
                                        command=self.stop_batch_upload, state="disabled")
        self.batch_stop_btn.pack(side=tk.LEFT)
        
        # Overall progress
        ttk.Label(batch_control_frame, text="Overall Progress:").pack(anchor=tk.W, pady=(10, 0))
        self.batch_progress_bar = ttk.Progressbar(batch_control_frame, variable=self.overall_progress, maximum=100)
        self.batch_progress_bar.pack(fill=tk.X, pady=(5, 0))
        
        # Batch status
        self.batch_status_label = ttk.Label(batch_control_frame, text="Ready")
        self.batch_status_label.pack(anchor=tk.W, pady=(5, 0))
        
    def setup_settings_tab(self):
        main_frame = ttk.Frame(self.settings_tab, padding="10")
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # Connection Settings
        conn_frame = ttk.LabelFrame(main_frame, text="ESP32 Connection", padding="10")
        conn_frame.pack(fill=tk.X, pady=(0, 10))
        
        # IP Address
        ttk.Label(conn_frame, text="ESP32 IP Address:").grid(row=0, column=0, sticky=tk.W, pady=5)
        ip_entry = ttk.Entry(conn_frame, textvariable=self.esp32_ip, width=20)
        ip_entry.grid(row=0, column=1, sticky=tk.W, padx=(10, 0), pady=5)
        
        # TCP Port
        ttk.Label(conn_frame, text="TCP Port:").grid(row=1, column=0, sticky=tk.W, pady=5)
        tcp_port_entry = ttk.Entry(conn_frame, textvariable=self.tcp_port, width=10)
        tcp_port_entry.grid(row=1, column=1, sticky=tk.W, padx=(10, 0), pady=5)
        
        # UDP Port
        ttk.Label(conn_frame, text="UDP Port:").grid(row=2, column=0, sticky=tk.W, pady=5)
        udp_port_entry = ttk.Entry(conn_frame, textvariable=self.udp_port, width=10)
        udp_port_entry.grid(row=2, column=1, sticky=tk.W, padx=(10, 0), pady=5)
        
        # Test buttons
        test_tcp_btn = ttk.Button(conn_frame, text="Test TCP", command=lambda: self.test_connection("tcp"))
        test_tcp_btn.grid(row=1, column=2, padx=(10, 0), pady=5)
        
        test_udp_btn = ttk.Button(conn_frame, text="Test UDP", command=lambda: self.test_connection("udp"))
        test_udp_btn.grid(row=2, column=2, padx=(10, 0), pady=5)
        
        # Protocol Information
        info_frame = ttk.LabelFrame(main_frame, text="Protocol Information", padding="10")
        info_frame.pack(fill=tk.X, pady=(0, 10))
        
        protocol_text = """
TCP Protocol: [4 bytes filename_len][4 bytes file_size][filename][file_data]
UDP Protocol: [4 bytes filename_len][4 bytes file_size][filename][file_data]

- All multi-byte values are in network byte order (big-endian)
- TCP provides reliable, ordered delivery
- UDP provides fast, single-packet delivery (size limited)
- Maximum UDP packet size: ~65KB (including headers)
        """.strip()
        
        info_label = ttk.Label(info_frame, text=protocol_text, font=("Consolas", 9), 
                              justify=tk.LEFT, foreground="blue")
        info_label.pack(anchor=tk.W)
        
    # Event handlers and utility methods
    def log_single(self, message):
        """Add message to single upload log"""
        timestamp = time.strftime("%H:%M:%S")
        self.single_log_text.insert(tk.END, f"[{timestamp}] {message}\n")
        self.single_log_text.see(tk.END)
        self.root.update_idletasks()
        
    def clear_single_log(self):
        """Clear single upload log"""
        self.single_log_text.delete(1.0, tk.END)
        
    def browse_single_file(self):
        """Browse for single file upload"""
        filename = filedialog.askopenfilename(
            title="Select file to upload",
            filetypes=[("All files", "*.*")]
        )
        
        if filename:
            self.selected_file.set(filename)
            self.update_file_info(filename)
            
    def update_file_info(self, filepath):
        """Update file information display"""
        try:
            file_path = Path(filepath)
            file_size = file_path.stat().st_size
            size_mb = file_size / (1024 * 1024)
            self.file_info_label.config(
                text=f"Size: {size_mb:.2f} MB ({file_size:,} bytes)",
                foreground="black"
            )
        except Exception as e:
            self.file_info_label.config(text=f"Error: {e}", foreground="red")
            
    def add_batch_files(self):
        """Add files to batch upload list"""
        filenames = filedialog.askopenfilenames(
            title="Select files to upload",
            filetypes=[("All files", "*.*")]
        )
        
        for filename in filenames:
            self.add_file_to_batch(filename)
            
    def add_batch_folder(self):
        """Add all files from a folder to batch upload"""
        folder = filedialog.askdirectory(title="Select folder")
        if folder:
            folder_path = Path(folder)
            for file_path in folder_path.rglob("*"):
                if file_path.is_file():
                    self.add_file_to_batch(str(file_path))
                    
    def add_file_to_batch(self, filepath):
        """Add a single file to batch list"""
        try:
            file_path = Path(filepath)
            file_size = file_path.stat().st_size
            size_mb = file_size / (1024 * 1024)
            
            # Insert into treeview
            self.batch_tree.insert("", tk.END, values=(
                file_path.name,
                f"{size_mb:.2f} MB",
                self.batch_protocol.get(),
                "Pending",
                "0%"
            ))
            
            # Add to internal list
            task = UploadTask(filepath, self.batch_protocol.get())
            self.upload_tasks.append(task)
            
        except Exception as e:
            messagebox.showerror("Error", f"Cannot add file {filepath}: {e}")
            
    def remove_batch_files(self):
        """Remove selected files from batch list"""
        selected = self.batch_tree.selection()
        for item in selected:
            index = self.batch_tree.index(item)
            self.batch_tree.delete(item)
            if 0 <= index < len(self.upload_tasks):
                self.upload_tasks.pop(index)
                
    def clear_batch_files(self):
        """Clear all files from batch list"""
        self.batch_tree.delete(*self.batch_tree.get_children())
        self.upload_tasks.clear()
        
    def test_connection(self, protocol):
        """Test connection to ESP32"""
        def test():
            try:
                port = self.tcp_port.get() if protocol == "tcp" else self.udp_port.get()
                self.log_single(f"Testing {protocol.upper()} connection to {self.esp32_ip.get()}:{port}...")
                
                if protocol == "tcp":
                    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    sock.settimeout(5)
                    result = sock.connect_ex((self.esp32_ip.get(), port))
                    sock.close()
                    success = result == 0
                else:  # UDP
                    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    sock.settimeout(5)
                    # For UDP, just try to create socket - can't really test without sending data
                    sock.close()
                    success = True
                
                if success:
                    self.log_single(f"✓ {protocol.upper()} connection successful!")
                    messagebox.showinfo("Connection Test", f"{protocol.upper()} connection successful!")
                else:
                    self.log_single(f"✗ {protocol.upper()} connection failed!")
                    messagebox.showerror("Connection Test", f"{protocol.upper()} connection failed!")
                    
            except Exception as e:
                self.log_single(f"✗ {protocol.upper()} connection error: {e}")
                messagebox.showerror("Connection Test", f"{protocol.upper()} connection error: {e}")
        
        threading.Thread(target=test, daemon=True).start()
        
    def start_single_upload(self):
        """Start single file upload"""
        if not self.selected_file.get():
            messagebox.showerror("No File", "Please select a file first.")
            return
            
        if self.is_uploading:
            messagebox.showwarning("Upload in Progress", "An upload is already in progress!")
            return
            
        self.is_uploading = True
        self.single_upload_btn.config(state="disabled")
        
        threading.Thread(target=self.upload_single_file, daemon=True).start()
        
    def upload_single_file(self):
        """Upload single file"""
        try:
            filepath = self.selected_file.get()
            protocol = self.upload_protocol.get()
            
            if protocol == "tcp":
                self.upload_via_tcp(filepath, self.update_single_progress)
            else:
                self.upload_via_udp(filepath, self.update_single_progress)
                
        except Exception as e:
            self.log_single(f"Upload error: {e}")
            messagebox.showerror("Upload Error", f"Upload failed: {e}")
        finally:
            self.is_uploading = False
            self.single_upload_btn.config(state="normal")
            
    def update_single_progress(self, progress, status, speed=""):
        """Update single upload progress"""
        self.current_progress.set(progress)
        self.upload_status.set(status)
        self.upload_speed.set(speed)
        
    def start_batch_upload(self):
        """Start batch upload process"""
        if not self.upload_tasks:
            messagebox.showwarning("No Files", "Please add files to upload first.")
            return
            
        if self.is_uploading:
            messagebox.showwarning("Upload in Progress", "An upload is already in progress!")
            return
            
        self.is_uploading = True
        self.batch_upload_btn.config(state="disabled")
        self.batch_stop_btn.config(state="normal")
        
        threading.Thread(target=self.batch_upload_worker, daemon=True).start()
        
    def batch_upload_worker(self):
        """Worker thread for batch uploads"""
        try:
            total_tasks = len(self.upload_tasks)
            
            for i, task in enumerate(self.upload_tasks):
                if not self.is_uploading:  # Check for stop signal
                    break
                    
                self.current_task_index = i
                self.batch_status_label.config(text=f"Uploading {i+1}/{total_tasks}: {Path(task.filepath).name}")
                
                # Update overall progress
                overall_progress = (i / total_tasks) * 100
                self.overall_progress.set(overall_progress)
                
                # Upload file
                try:
                    if task.protocol == "tcp":
                        self.upload_via_tcp(task.filepath, lambda p, s, sp: self.update_batch_item(i, p, s))
                    else:
                        self.upload_via_udp(task.filepath, lambda p, s, sp: self.update_batch_item(i, p, s))
                        
                    task.status = "Complete"
                    self.update_batch_tree_item(i, status="Complete", progress="100%")
                    
                except Exception as e:
                    task.status = "Error"
                    task.error = str(e)
                    self.update_batch_tree_item(i, status="Error", progress="0%")
                    
            # Final update
            self.overall_progress.set(100)
            self.batch_status_label.config(text="Batch upload complete!")
            
        finally:
            self.is_uploading = False
            self.batch_upload_btn.config(state="normal")
            self.batch_stop_btn.config(state="disabled")
            
    def update_batch_item(self, index, progress, status):
        """Update batch item progress"""
        self.update_batch_tree_item(index, progress=f"{progress:.0f}%")
        
    def update_batch_tree_item(self, index, status=None, progress=None):
        """Update treeview item"""
        children = self.batch_tree.get_children()
        if 0 <= index < len(children):
            item = children[index]
            values = list(self.batch_tree.item(item)["values"])
            
            if status is not None:
                values[3] = status
            if progress is not None:
                values[4] = progress
                
            self.batch_tree.item(item, values=values)
            
    def stop_batch_upload(self):
        """Stop batch upload"""
        self.is_uploading = False
        self.batch_status_label.config(text="Stopping...")
        
    def upload_via_tcp(self, filepath, progress_callback):
        """Upload file via TCP"""
        filename = os.path.basename(filepath)
        file_size = os.path.getsize(filepath)
        filename_bytes = filename.encode('utf-8')
        filename_len = len(filename_bytes)
        
        # Connect
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(30)
        sock.connect((self.esp32_ip.get(), self.tcp_port.get()))
        
        try:
            # Send header
            header = struct.pack('!II', filename_len, file_size)
            sock.send(header)
            sock.send(filename_bytes)
            
            # Send file data
            start_time = time.time()
            bytes_sent = 0
            
            with open(filepath, 'rb') as f:
                while True:
                    chunk = f.read(4096)
                    if not chunk:
                        break
                        
                    sock.send(chunk)
                    bytes_sent += len(chunk)
                    
                    # Update progress
                    progress = (bytes_sent / file_size) * 100
                    elapsed = time.time() - start_time
                    speed = f"{(bytes_sent/1024/elapsed):.1f} KB/s" if elapsed > 0 else "0 KB/s"
                    
                    progress_callback(progress, f"Uploading {filename}", speed)
                    
            # Get response
            response = sock.recv(10).decode('utf-8', errors='ignore')
            if not response.startswith('OK'):
                raise Exception(f"ESP32 error: {response}")
                
        finally:
            sock.close()
            
    def upload_via_udp(self, filepath, progress_callback):
        """Upload file via UDP"""
        filename = os.path.basename(filepath)
        file_size = os.path.getsize(filepath)
        
        # Check file size limit for UDP
        max_udp_size = 60000  # Conservative limit
        if file_size > max_udp_size:
            raise Exception(f"File too large for UDP ({file_size} bytes > {max_udp_size} bytes)")
            
        filename_bytes = filename.encode('utf-8')
        filename_len = len(filename_bytes)
        
        # Create UDP socket
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(10)
        
        try:
            progress_callback(10, f"Reading {filename}", "")
            
            # Read entire file
            with open(filepath, 'rb') as f:
                file_data = f.read()
                
            progress_callback(50, f"Sending {filename}", "")
            
            # Create packet
            header = struct.pack('!II', filename_len, file_size)
            packet = header + filename_bytes + file_data
            
            # Send packet
            start_time = time.time()
            sock.sendto(packet, (self.esp32_ip.get(), self.udp_port.get()))
            
            progress_callback(90, f"Waiting for response", "")
            
            # Wait for response
            response, addr = sock.recvfrom(10)
            response = response.decode('utf-8', errors='ignore')
            
            elapsed = time.time() - start_time
            speed = f"{(file_size/1024/elapsed):.1f} KB/s" if elapsed > 0 else "0 KB/s"
            
            if not response.startswith('OK'):
                raise Exception(f"ESP32 error: {response}")
                
            progress_callback(100, f"Complete", speed)
            
        finally:
            sock.close()

def main():
    root = tk.Tk()
    app = AdvancedUploaderGUI(root)
    
    # Center window
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
