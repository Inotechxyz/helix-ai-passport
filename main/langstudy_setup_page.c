// main/langstudy_setup_page.c
#include "langstudy_setup_page.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *out;
    size_t cap;
    size_t used;
    bool overflow;
} writer_t;

static void w_str(writer_t *w, const char *text)
{
    if (w->overflow || !text) return;
    size_t len = strlen(text);
    if (w->used + len + 1 > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->out + w->used, text, len);
    w->used += len;
    w->out[w->used] = '\0';
}

static void w_escaped(writer_t *w, const char *text)
{
    if (!text) return;
    for (const char *p = text; *p && !w->overflow; p++) {
        switch (*p) {
        case '&': w_str(w, "&amp;"); break;
        case '<': w_str(w, "&lt;"); break;
        case '>': w_str(w, "&gt;"); break;
        case '"': w_str(w, "&quot;"); break;
        case '\'': w_str(w, "&#39;"); break;
        default: {
            char one[2] = { *p, '\0' };
            w_str(w, one);
            break;
        }
        }
    }
}

size_t ls_setup_page_render(char *out, size_t cap,
                            const ls_config_t *config,
                            const char *ap_ssid, const char *setup_url)
{
    if (!out || cap == 0 || !config) return 0;
    out[0] = '\0';

    writer_t w = { .out = out, .cap = cap, .used = 0, .overflow = false };

    w_str(&w,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>LangStudy Setup</title><style>"
        "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
        "margin:0;padding:18px;background:#f4efe3;color:#1e2a38}"
        "h1{font-size:20px;margin:0 0 4px}"
        ".sub{color:#6b7280;font-size:13px;margin-bottom:16px}"
        "form{background:#fff;border:1px solid #d9d2c3;border-radius:12px;"
        "padding:14px;margin-bottom:14px}"
        "label{display:block;font-size:12px;font-weight:600;margin:10px 0 4px;"
        "text-transform:uppercase;letter-spacing:.04em;color:#4b5563}"
        "input[type=text],input[type=password]{width:100%;box-sizing:border-box;"
        "padding:10px;border:1px solid #cbd5e1;border-radius:8px;font-size:15px}"
        "button{margin-top:14px;width:100%;padding:11px;border:0;border-radius:8px;"
        "background:#16a085;color:#fff;font-size:15px;font-weight:600}"
        ".ghost{background:#fff;color:#b91c1c;border:1px solid #fca5a5}"
        ".hint{font-size:12px;color:#6b7280;line-height:1.45}"
        ".pill{display:inline-block;padding:3px 8px;border-radius:999px;"
        "background:#e8f5f1;color:#0f766e;font-size:12px;font-weight:600}"
        ".msg{font-size:13px;margin:8px 0 0;color:#334155;min-height:18px}"
        "</style></head><body>");

    w_str(&w, "<h1>PASSPORT ENGLISH</h1><div class=\"sub\">Device setup</div>");
    w_str(&w, "<p class=\"hint\">Connect this phone to <b>");
    w_escaped(&w, ap_ssid);
    w_str(&w, "</b> and open <b>");
    w_escaped(&w, setup_url);
    w_str(&w, "</b>.</p>");

    w_str(&w, "<form method=\"post\" action=\"/save\">");
    w_str(&w, "<label>Wi-Fi name (2.4 GHz)</label>");
    w_str(&w, "<input type=\"text\" name=\"ssid\" maxlength=\"32\" value=\"");
    w_escaped(&w, config->wifi_ssid);
    w_str(&w, "\">");

    w_str(&w, "<label>Wi-Fi password</label>");
    w_str(&w, "<input type=\"password\" name=\"pass\" maxlength=\"63\" "
              "autocomplete=\"new-password\">");
    w_str(&w, "<p class=\"hint\">Leave blank to keep the saved password when "
              "the Wi-Fi name is unchanged. A blank password for a new name "
              "means an open network.</p>");

    w_str(&w, "<label>DeepSeek API key</label>");
    w_str(&w, "<input type=\"password\" name=\"api\" maxlength=\"200\" "
              "placeholder=\"");
    if (ls_config_has_api_key(config)) {
        w_str(&w, "saved (leave blank to keep)");
    } else {
        w_str(&w, "sk-...");
    }
    w_str(&w, "\">");
    if (ls_config_has_api_key(config)) {
        w_str(&w, "<p class=\"hint\"><label style=\"display:inline;"
                  "text-transform:none;letter-spacing:0\">"
                  "<input type=\"checkbox\" name=\"clear_api\" value=\"1\"> "
                  "Delete the saved API key</label></p>");
    }

    w_str(&w, "<label>Sync server URL (optional)</label>");
    w_str(&w, "<input type=\"text\" name=\"sync\" maxlength=\"160\" "
              "placeholder=\"http://192.168.1.10:8080/progress\" value=\"");
    w_escaped(&w, config->sync_url);
    w_str(&w, "\">");

    w_str(&w, "<button type=\"submit\">Save and connect</button></form>");

    w_str(&w, "<form method=\"post\" action=\"/clear\">"
              "<button class=\"ghost\" type=\"submit\">"
              "Forget Wi-Fi network</button></form>");

    w_str(&w, "<p class=\"hint\">Connection status: "
              "<span class=\"pill\" id=\"state\">idle</span></p>"
              "<p class=\"msg\" id=\"status\">Ready. Saving will test the "
              "Wi-Fi connection before it is stored.</p>");

    w_str(&w,
        "<script>"
        "async function poll(){"
        "try{const r=await fetch('/status',{cache:'no-store'});"
        "const j=await r.json();"
        "document.getElementById('state').textContent=j.state;"
        "document.getElementById('status').textContent=j.message;"
        "}catch(e){}"
        "}"
        "poll();setInterval(poll,1500);"
        "</script></body></html>");

    if (w.overflow) {
        out[0] = '\0';
        return 0;
    }
    return w.used;
}
