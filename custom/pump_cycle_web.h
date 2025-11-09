#pragma once

#include "esphome.h"
#include "esphome/components/web_server/web_server.h"

namespace esphome {
namespace custom_pump_cycle {

// Простая машина состояний для циклической работы помпы с веб-страницей управления
class PumpCycleWeb : public Component {
 public:
  PumpCycleWeb(fan::Fan *pump,
               int *on_minutes_ptr,
               int *off_minutes_ptr,
               bool *enabled_ptr,
               web_server::WebServer *server)
      : pump_(pump), on_minutes_ptr_(on_minutes_ptr), off_minutes_ptr_(off_minutes_ptr),
        enabled_ptr_(enabled_ptr), server_(server) {}

  void setup() override {
    this->last_change_ms_ = millis();
    this->running_on_ = false;

    // Регистрация HTTP эндпоинтов
    if (this->server_ != nullptr) {
      // Страница управления
      this->server_->on("/pump-cycle", web_server_idf::HTTP_GET, [this](web_server_idf::AsyncWebServerRequest *req) {
        std::string html = R"HTML(
<!doctype html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Pump Cycle</title>
<style>body{font-family:system-ui,Segoe UI,Roboto,Arial;background:#111;color:#eee;margin:16px}input,button{font-size:16px;padding:8px;margin:6px}label{display:block;margin-top:10px} .row{margin:8px 0}</style>
</head><body>
<h2>Pump Cycle</h2>
<div class=row>
  <label><input type='checkbox' id='enabled'> Enabled</label>
  <label>ON, min <input id='on' type='number' min='1' max='120'></label>
  <label>OFF, min <input id='off' type='number' min='1' max='120'></label>
  <div class=row>
    <button id='save'>Save</button>
    <span id='status'></span>
  </div>
</div>
<script>
async function load(){
  const r=await fetch('/api/pump-cycle');
  const j=await r.json();
  enabled.checked=j.enabled;
  on.value=j.on;
  off.value=j.off;
}
async function save(){
  const params=new URLSearchParams({enabled:enabled.checked?1:0,on:on.value,off:off.value});
  const r=await fetch('/api/pump-cycle?'+params.toString(),{method:'POST'});
  status.textContent=r.ok?'Saved':'Error';
  setTimeout(()=>status.textContent='',1500);
}
saveBtn=save; document.getElementById('save').onclick=save; load();
</script>
</body></html>)HTML";
        req->send(200, "text/html", html.c_str());
      });

      // Получить текущее состояние
      this->server_->on("/api/pump-cycle", web_server_idf::HTTP_GET, [this](web_server_idf::AsyncWebServerRequest *req) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"on\":%d,\"off\":%d}",
                 (*this->enabled_ptr_) ? "true" : "false", *this->on_minutes_ptr_, *this->off_minutes_ptr_);
        req->send(200, "application/json", buf);
      });

      // Установить состояние
      this->server_->on("/api/pump-cycle", web_server_idf::HTTP_POST, [this](web_server_idf::AsyncWebServerRequest *req) {
        // enabled, on, off из query
        bool enabled = *this->enabled_ptr_;
        int onm = *this->on_minutes_ptr_;
        int offm = *this->off_minutes_ptr_;

        auto get_int = [&](const char *name, int def) -> int {
          auto p = req->getParam(name, true /* get from post or query */);
          if (p) return atoi(p->value().c_str());
          return def;
        };
        auto get_bool = [&](const char *name, bool def) -> bool {
          auto p = req->getParam(name, true);
          if (p) return (p->value() == "1" || p->value() == "true");
          return def;
        };

        enabled = get_bool("enabled", enabled);
        onm = get_int("on", onm);
        offm = get_int("off", offm);

        onm = std::max(1, std::min(120, onm));
        offm = std::max(1, std::min(120, offm));

        *this->on_minutes_ptr_ = onm;
        *this->off_minutes_ptr_ = offm;
        *this->enabled_ptr_ = enabled;

        if (enabled) this->start_cycle_(); else this->stop_cycle_();
        req->send(200, "application/json", "{\"ok\":true}");
      });
    }
  }

  void loop() override {
    if (!*this->enabled_ptr_) return;
    const uint32_t now = millis();
    const uint32_t period_ms = (this->running_on_ ? (*this->on_minutes_ptr_) : (*this->off_minutes_ptr_)) * 60000UL;
    if (now - this->last_change_ms_ >= period_ms) {
      this->running_on_ = !this->running_on_;
      this->last_change_ms_ = now;
      if (this->running_on_) {
        this->pump_->turn_on().perform();
        ESP_LOGI("pump_cycle", "Pump ON (custom)");
      } else {
        this->pump_->turn_off().perform();
        ESP_LOGI("pump_cycle", "Pump OFF (custom)");
      }
    }
  }

  void dump_config() override {
    ESP_LOGCONFIG("pump_cycle", "PumpCycleWeb: on=%d min, off=%d min, enabled=%s",
                  *this->on_minutes_ptr_, *this->off_minutes_ptr_, *this->enabled_ptr_ ? "true" : "false");
  }

 private:
  void start_cycle_() {
    this->last_change_ms_ = millis();
    // Стартуем с ON
    this->running_on_ = true;
    this->pump_->turn_on().perform();
  }
  void stop_cycle_() {
    this->pump_->turn_off().perform();
    this->running_on_ = false;
  }

  fan::Fan *pump_;
  int *on_minutes_ptr_{nullptr};
  int *off_minutes_ptr_{nullptr};
  bool *enabled_ptr_{nullptr};
  web_server::WebServer *server_{nullptr};

  bool running_on_{false};
  uint32_t last_change_ms_{0};
};

}  // namespace custom_pump_cycle
}  // namespace esphome




