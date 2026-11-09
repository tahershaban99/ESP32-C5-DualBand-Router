#!/usr/bin/env python3
"""
Automated Assembler for ESP32 Advanced Glassmorphic Web UI
Combines modular HTML, CSS, and JS components from web_src/ into web_page.html and generates web_page_gz.h.
"""

import os
import sys
import re
import time
import gzip
import argparse

CRITICAL_DOM_IDS = [
    "tab-overview",
    "tab-devices",
    "tab-period",
    "tab-telegram",
    "tab-reboot",
    "tab-settings",
    "tab-ota",
    "login-modal",
    "live-floating-toast"
]

CRITICAL_JS_FUNCTIONS = [
    "refreshData",
    "renderDevices",
    "switchTab",
    "updateOverviewTab",
    "uploadOtaFirmware",
    "submitTelegramSettings",
    "scanWifiNetworks"
]

def resolve_paths(project_root=None):
    if project_root is None:
        project_root = os.path.dirname(os.path.abspath(__file__))

    # Look for web_src either in main/web_src or web_src
    candidates = [
        (os.path.join(project_root, "main", "web_src"), os.path.join(project_root, "main", "web_page.html"), os.path.join(project_root, "main", "web_page_gz.h")),
        (os.path.join(project_root, "web_src"), os.path.join(project_root, "web_page.html"), os.path.join(project_root, "web_page_gz.h")),
    ]

    for src_dir, html_file, gz_file in candidates:
        template_file = os.path.join(src_dir, "index.template.html")
        if os.path.exists(template_file):
            return src_dir, template_file, html_file, gz_file

    raise FileNotFoundError("Could not find web_src/index.template.html in main/web_src or ./web_src")

def assemble_webpage(src_dir, template_file, output_html):
    with open(template_file, "r", encoding="utf-8") as f:
        template_content = f.read()

    include_pattern = re.compile(r"\{\{INCLUDE:([^\}]+)\}\}")

    def replace_include(match):
        rel_path = match.group(1).strip()
        full_path = os.path.join(src_dir, rel_path)
        if not os.path.exists(full_path):
            raise FileNotFoundError(f"Missing component file: {full_path}")
        with open(full_path, "r", encoding="utf-8") as f:
            return f.read().rstrip("\r\n")

    assembled_content = include_pattern.sub(replace_include, template_content)

    # Post-build verification check
    unresolved = include_pattern.findall(assembled_content)
    if unresolved:
        raise ValueError(f"Build Failed: Unresolved include tags remain: {unresolved}")

    # Verify critical DOM IDs
    missing_ids = []
    for dom_id in CRITICAL_DOM_IDS:
        # Match id="..." or id='...'
        if not re.search(r'id=["\']' + re.escape(dom_id) + r'["\']', assembled_content):
            missing_ids.append(dom_id)
    if missing_ids:
        raise ValueError(f"Build Verification Error: Missing critical DOM IDs: {missing_ids}")

    # Verify critical JS functions
    missing_fns = []
    for fn in CRITICAL_JS_FUNCTIONS:
        if not re.search(r'(?:function\s+' + re.escape(fn) + r'|async\s+function\s+' + re.escape(fn) + r'|' + re.escape(fn) + r'\s*=\s*(?:function|async\s+function|\())', assembled_content):
            missing_fns.append(fn)
    if missing_fns:
        raise ValueError(f"Build Verification Error: Missing critical JS functions: {missing_fns}")

    # Write output
    with open(output_html, "w", encoding="utf-8") as f:
        f.write(assembled_content)

    return assembled_content

def generate_gzip_header(output_html, output_gz_header):
    with open(output_html, "rb") as f:
        raw_bytes = f.read()

    gz_bytes = gzip.compress(raw_bytes, compresslevel=9)

    with open(output_gz_header, "w", encoding="utf-8") as f:
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n#include <stddef.h>\n\n")
        f.write("// Pre-compressed Glassmorphic Web UI stored strictly in Flash .rodata (Zero RAM usage)\n")
        f.write("static const uint8_t PAGE_GZ[] = {\n")
        for i, b in enumerate(gz_bytes):
            if i % 16 == 0:
                f.write("  ")
            f.write(f"0x{b:02x}, ")
            if (i + 1) % 16 == 0:
                f.write("\n")
        if len(gz_bytes) % 16 != 0:
            f.write("\n")
        f.write("};\n\n")
        f.write(f"static const size_t PAGE_GZ_LEN = {len(gz_bytes)};\n")

    return len(raw_bytes), len(gz_bytes)

def get_mtimes(src_dir):
    mtimes = {}
    for root, _, files in os.walk(src_dir):
        for f in files:
            path = os.path.join(root, f)
            try:
                mtimes[path] = os.path.getmtime(path)
            except OSError:
                pass
    return mtimes

def build(compress=True):
    src_dir, template_file, output_html, output_gz_header = resolve_paths()
    start_time = time.time()
    content = assemble_webpage(src_dir, template_file, output_html)
    elapsed = (time.time() - start_time) * 1000

    line_count = len(content.splitlines())
    byte_count = len(content.encode("utf-8"))

    print(f"✨ [SUCCESS] Assembled: {output_html}")
    print(f"   Lines: {line_count:,} | Size: {byte_count:,} bytes | Build Time: {elapsed:.1f}ms")
    print(f"   Verification: All {len(CRITICAL_DOM_IDS)} DOM IDs & {len(CRITICAL_JS_FUNCTIONS)} JS functions validated.")

    if compress:
        raw_len, gz_len = generate_gzip_header(output_html, output_gz_header)
        ratio = (gz_len / raw_len) * 100 if raw_len else 0
        print(f"📦 [GZIP] Generated: {output_gz_header} ({gz_len:,} bytes, {ratio:.1f}% ratio)")

def watch_mode(compress=True):
    src_dir, template_file, output_html, output_gz_header = resolve_paths()
    print(f"👀 Watching {src_dir} for changes (poll: 0.5s)... Press Ctrl+C to stop.")
    
    # Initial build
    try:
        build(compress=compress)
    except Exception as e:
        print(f"❌ [BUILD ERROR]: {e}")

    last_mtimes = get_mtimes(src_dir)
    try:
        while True:
            time.sleep(0.5)
            current_mtimes = get_mtimes(src_dir)
            if current_mtimes != last_mtimes:
                last_mtimes = current_mtimes
                print("\n🔄 Detected changes in web_src/, rebuilding...")
                try:
                    build(compress=compress)
                except Exception as e:
                    print(f"❌ [BUILD ERROR]: {e}")
    except KeyboardInterrupt:
        print("\n🛑 Stopped watch mode.")

def main():
    parser = argparse.ArgumentParser(description="Assemble modular web_src/ into web_page.html")
    parser.add_argument("--watch", "-w", action="store_true", help="Watch web_src/ directory for changes and rebuild automatically")
    parser.add_argument("--no-compress", action="store_true", help="Skip generating web_page_gz.h header file")
    args = parser.parse_args()

    compress = not args.no_compress
    if args.watch:
        watch_mode(compress=compress)
    else:
        build(compress=compress)

if __name__ == "__main__":
    main()

