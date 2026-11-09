# 🤖 AGENTS.md — Web UI Architecture & Development Guide

> **TARGET AUDIENCE:** AI Coding Agents & Human Developers  
> **SCOPE:** Web UI subsystem for the ESP32-C5 Dual-Band Glassmorphic Router

---

## 1. ⚠️ CARDINAL RULE (Zero Direct Artifact Editing)

```text
       ✍️ EDIT HERE ONLY                     ⚙️ COMPILE                     🚫 DO NOT EDIT
  ┌─────────────────────────┐             ┌───────────────┐             ┌─────────────────────┐
  │     main/web_src/       │  ─────────> │ build_web.py  │  ─────────> │ main/web_page.html   │
  │ (HTML, CSS, JS Partials)│             └───────────────┘             │ main/web_page_gz.h  │
  └─────────────────────────┘                                           │ preview.html        │
                                                                        └─────────────────────┘
```

1. **NEVER edit `main/web_page.html` or `preview.html` directly.** They are compiled build artifacts (~6,400 lines). Any direct edit will be overwritten and lost.
2. **NEVER edit `main/web_page_gz.h` directly.** It is auto-generated Gzip byte code for ESP-IDF flash.
3. **NEVER use static hardcoded colors in HTML/CSS.** Strictly use variables from `main/design_tokens.css` (e.g. `var(--bg-main)`, `var(--glass-bg)`, `var(--accent)`, `var(--green)`).
4. **ALWAYS edit inside `main/web_src/`.**
5. **ALWAYS execute `python build_web.py`** after making changes or adding files.

---

## 2. 🏗️ ARCHITECTURE & DIRECTORY MAP

All modular source code lives in [`main/web_src/`](file:///c:/Users/Mohamed/Desktop/Esp32-C5%20Router/main/web_src/):

```text
main/web_src/
├── index.template.html          # Master shell template with {{INCLUDE:...}} placeholders
│
├── css/                         # STYLESHEETS (Strict cascade order matching design system)
│   ├── 00_design_tokens.css     # Clean solid anti-AI-slop tokens (:root, dark, light surfaces & variables)
│   ├── 01_themes_and_base.css   # Keyframe animations, base typography, reset, .glass, zoom controller
│   ├── 02_layout_and_nav.css    # Header, sticky bar, sidebar, tabs, metrics grid, responsive layout
│   ├── 03_components_modals.css # Telegram dashboard, forms, buttons, inputs, modals, lockout banner, presets
│   ├── 04_devices_and_table.css # Cards & table view, quotas, device capsules, action icon buttons
│   └── 05_overview_and_toast.css# Floating live toast, pulse-dot, overview 3-col grid, gauges, sparklines
│
├── html/                        # STRUCTURAL PARTIALS (One file per visual component)
│   ├── header_and_metrics.html  # Sticky top header, theme switcher, WAN status, 7 metric cards
│   ├── sidebar.html             # Vertical sidebar navigation with tab buttons
│   ├── tab_overview.html        # Overview dashboard tab section
│   ├── tab_devices.html         # Connected devices management tab section
│   ├── tab_period.html          # Monthly quota & renewal cycle tab section
│   ├── tab_telegram.html        # Telegram bot configuration & chat simulator tab section
│   ├── tab_reboot.html          # Auto-reboot scheduler & timer tab section
│   ├── tab_settings.html        # System settings & WiFi manager tab section
│   ├── tab_ota.html             # Firmware OTA updater tab section
│   └── modals_and_toast.html    # Live floating toast notification and admin login modal
│
└── js/                          # CLIENT-SIDE LOGIC (Separated by feature)
    ├── 01_core_ui_and_auth.js   # Zoom, toasts, formatters, theme switch, tab routing, auth & login/logout
    ├── 02_charts_and_overview.js# Canvas speed chart, overview dashboard updates, sparklines, refreshData loop
    ├── 03_devices_crud.js       # renderDevices, toggleGaming, toggleBlock, reset, delete, save limits
    ├── 04_ota_and_quota.js      # OTA upload, cloud checks, period days, quota reset & configuration
    ├── 05_telegram_bot.js       # Chat simulator, formatters, Telegram alerts, discovered users, add admin
    └── 06_system_and_wifi.js    # CPU presets, reboot countdowns, meters, admin password, WiFi scanner, init
```

---

## 3. 🎯 WHERE TO MAKE CHANGES?

When asked to modify a feature, use this quick reference table to find the exact files:

| Feature / Area | CSS File | HTML Partial | JS File |
| :--- | :--- | :--- | :--- |
| **Color Tokens & Theme Variables** | `css/00_design_tokens.css` (or `main/design_tokens.css`) | *(No static colors allowed)* | *(Tokens via `getToken()`)* |
| **Themes, Zoom & Base UI** | `css/01_themes_and_base.css` | `html/header_and_metrics.html` | `js/01_core_ui_and_auth.js` |
| **Layout, Header, Metrics**| `css/02_layout_and_nav.css` | `html/header_and_metrics.html` | `js/01_core_ui_and_auth.js` |
| **Sidebar Navigation** | `css/02_layout_and_nav.css` | `html/sidebar.html` | `js/01_core_ui_and_auth.js` |
| **Overview Tab & Gauges** | `css/05_overview_and_toast.css` | `html/tab_overview.html` | `js/02_charts_and_overview.js` |
| **Speed History Chart** | `css/02_layout_and_nav.css` | `html/tab_overview.html` | `js/02_charts_and_overview.js` |
| **Devices Management & Table** | `css/04_devices_and_table.css` | `html/tab_devices.html` | `js/03_devices_crud.js` |
| **Quota & Billing Period** | `css/03_components_modals.css` | `html/tab_period.html` | `js/04_ota_and_quota.js` |
| **Telegram Bot & Chat** | `css/03_components_modals.css` | `html/tab_telegram.html` | `js/05_telegram_bot.js` |
| **Scheduled / Auto Reboot**| `css/03_components_modals.css` | `html/tab_reboot.html` | `js/06_system_and_wifi.js` |
| **WiFi & System Settings** | `css/03_components_modals.css` | `html/tab_settings.html` | `js/06_system_and_wifi.js` |
| **OTA Firmware Updates** | `css/03_components_modals.css` | `html/tab_ota.html` | `js/04_ota_and_quota.js` |
| **Login Modal & Toasts** | `css/03_components_modals.css` / `05_overview_and_toast.css` | `html/modals_and_toast.html` | `js/01_core_ui_and_auth.js` |

---

## 4. 🧩 HOW TO ADD A NEW COMPONENT

> [!IMPORTANT]
> **Single Component Principle:** NEVER lump multiple distinct features into an existing file. Every new component must have dedicated files in `html/`, `css/`, and `js/`.

### Step-by-Step Procedure:

1. **Create HTML Partial:**
   - Create `main/web_src/html/my_new_feature.html`.
   - Write clean, semantic HTML with descriptive IDs and classes. Use **only design tokens** (`var(--...)`), NO static colors!
   ```html
   <section id="tab-new-feature" class="tab-content">
     <div class="card-section glass">
       <h2>عنوان الميزة الجديدة</h2>
       <!-- Component content -->
     </div>
   </section>
   ```

2. **Create CSS Stylesheet:**
   - Create `main/web_src/css/my_new_feature.css` (or append to a logically numbered CSS file).
   - Use CSS variables (`var(--glass-bg)`, `var(--accent)`, `var(--text)`, etc.) to match the solid design tokens.

3. **Create JS Module:**
   - Create `main/web_src/js/my_new_feature.js`.
   - **DO NOT** use ES module `export`/`import` or wrapping IIFEs that isolate scope.
   - Define functions as standard top-level functions so inline HTML handlers (`onclick="myHandler()"`) work.

4. **Register in `main/web_src/index.template.html`:**
   - Add the CSS placeholder inside `<style>`:
     ```html
     {{INCLUDE:css/my_new_feature.css}}
     ```
   - Add the HTML placeholder inside `<div class="content-area">`:
     ```html
     {{INCLUDE:html/my_new_feature.html}}
     ```
   - Add the JS placeholder inside `<script>`:
     ```html
     {{INCLUDE:js/my_new_feature.js}}
     ```

5. **Assemble & Validate:**
   - Run the assembler script from the root directory:
     ```powershell
     python build_web.py
     ```
   - Verify that all DOM IDs and functions pass validation.

---

## 5. 🛡️ STRICT INVARIANTS & CODING RULES

1. **Zero Static Colors:**
   - Always use CSS tokens from `main/design_tokens.css`. Never write inline `#hex` or `rgba(...)` styles in HTML markup.
2. **UTF-8 & Arabic RTL Integrity:**
   - All files must be saved with `encoding="utf-8"`.
   - Preserve Arabic directionality (`dir="rtl"`), typography (`Cairo` font), and emojis.
3. **Global Window Scope:**
   - Do NOT wrap JS files in closures or ES modules that hide functions from the global `window` scope. Inline HTML event handlers (`onclick`, `onsubmit`, `onchange`) depend on global access.
4. **Triple Output Requirement:**
   - `build_web.py` produces `main/web_page.html`, `preview.html` (for direct inspection), and `main/web_page_gz.h` (level 9 compressed C header for ESP-IDF flash). All stay in sync.

---

## 6. 🚀 CLI WORKFLOW & COMMANDS

### Assemble Once
```powershell
python build_web.py
```
*Outputs line count, size, validation status, and generates `web_page_gz.h` and `preview.html`.*

### Live Development (Watch Mode)
```powershell
python build_web.py --watch
```
*Watches `main/web_src/` and `main/design_tokens.css` for file modifications every 0.5s and recompiles automatically on save.*

### Full Firmware Build
```powershell
.\build_firmware.ps1
```
*Automatically triggers `python build_web.py` before invoking Ninja and ESP-IDF.*
