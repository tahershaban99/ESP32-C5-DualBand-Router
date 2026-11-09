import gzip
import os
import re

base_dir = os.path.dirname(__file__)
html_path = os.path.join(base_dir, "main", "web_page.html")
tokens_path = os.path.join(base_dir, "main", "design_tokens.css")
preview_path = os.path.join(base_dir, "preview.html")
gz_header_path = os.path.join(base_dir, "main", "web_page_gz.h")

# 1. Read source web_page.html
with open(html_path, "r", encoding="utf-8") as f:
    html_content = f.read()

# 2. Check for external design_tokens.css link and inline it for production single-file serving
link_pattern = re.compile(r'<link\s+rel=["\']stylesheet["\']\s+href=["\']design_tokens\.css["\']\s*/?>', re.IGNORECASE)

if link_pattern.search(html_content) and os.path.exists(tokens_path):
    with open(tokens_path, "r", encoding="utf-8") as f:
        tokens_content = f.read()
    
    inlined_tokens = f"<style>\n/* Bundled from main/design_tokens.css for ESP32 standalone flash storage */\n{tokens_content}\n</style>"
    bundled_html = link_pattern.sub(lambda _: inlined_tokens, html_content, count=1)
    print("Inlined design_tokens.css into HTML for single-route flash serving.")
else:
    bundled_html = html_content
    print("No external design_tokens.css link found or already bundled.")

# 3. Write preview.html
with open(preview_path, "w", encoding="utf-8") as f:
    f.write(bundled_html)
print(f"Updated {preview_path} for preview.")

# 4. Maximum compression level 9
raw_bytes = bundled_html.encode("utf-8")
gz_bytes = gzip.compress(raw_bytes, compresslevel=9)

print(f"Original HTML size: {len(raw_bytes)} bytes")
print(f"Gzip compressed size: {len(gz_bytes)} bytes (ratio: {len(gz_bytes)/len(raw_bytes)*100:.1f}%)")

# 5. Generate C header
with open(gz_header_path, "w", encoding="utf-8") as f:
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

print(f"Generated {gz_header_path} successfully!")
