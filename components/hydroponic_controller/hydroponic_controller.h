#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/fan/fan.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/web_server/web_server.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/web_server_idf/web_server_idf.h"

namespace esphome {
namespace hydroponic_controller {

static const char *const TAG = "hydroponic_controller";

// Структура для хранения настроек в энергонезависимой памяти
struct Settings {
  bool enabled = false;
  int on_minutes = 5;
  int off_minutes = 15;
  bool light_sched_enabled = false;
  int light_on_minutes = 1080;   // 18:00
  int light_off_minutes = 540;   // 09:00
  uint32_t crc = 0;
  
  uint32_t calculate_crc() const {
    uint32_t hash = 2166136261u;
    const uint8_t *data = reinterpret_cast<const uint8_t*>(this);
    for (size_t i = 0; i < sizeof(Settings) - sizeof(crc); i++) {
      hash ^= data[i];
      hash *= 16777619u;
    }
    return hash;
  }
};

class HydroponicController : public Component {
 public:
  void set_pump(fan::Fan *pump) { pump_ = pump; }
  void set_light(light::LightState *light) { light_ = light; }
  void set_clock(time::RealTimeClock *clock) { clock_ = clock; }
  void set_server(web_server::WebServer *server) { server_ = server; }
  void set_durations(int on_min, int off_min) { 
    on_minutes_ = on_min; 
    off_minutes_ = off_min; 
  }
  void set_enabled(bool e) { enabled_ = e; }

  void setup() override {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, ">>> Setting up Hydroponic Controller...");
    
    // Загрузка сохранённых настроек из энергонезависимой памяти
    pref_ = global_preferences->make_preference<Settings>(fnv1_hash("hydro_settings"));
    Settings loaded;
    ESP_LOGI(TAG, "Attempting to load settings from NVS...");
    if (pref_.load(&loaded)) {
      ESP_LOGI(TAG, "Settings found in NVS, checking CRC...");
      if (loaded.crc == loaded.calculate_crc()) {
        enabled_ = loaded.enabled;
        on_minutes_ = loaded.on_minutes;
        off_minutes_ = loaded.off_minutes;
        light_sched_enabled_ = loaded.light_sched_enabled;
        light_on_minutes_ = loaded.light_on_minutes;
        light_off_minutes_ = loaded.light_off_minutes;
        ESP_LOGI(TAG, "✓ Settings loaded from flash successfully!");
        ESP_LOGI(TAG, "  Pump schedule: %s, ON=%dmin, OFF=%dmin", 
                 enabled_ ? "enabled" : "disabled", on_minutes_, off_minutes_);
        ESP_LOGI(TAG, "  Light schedule: %s, ON=%02d:%02d, OFF=%02d:%02d",
                 light_sched_enabled_ ? "enabled" : "disabled",
                 light_on_minutes_/60, light_on_minutes_%60,
                 light_off_minutes_/60, light_off_minutes_%60);
      } else {
        ESP_LOGW(TAG, "✗ Settings CRC mismatch, using defaults");
      }
    } else {
      ESP_LOGI(TAG, "No saved settings found, using defaults");
    }
    
    last_change_ms_ = millis();
    running_on_ = false;
    register_routes_();
    if (enabled_) start_cycle_();
    ESP_LOGI(TAG, ">>> Hydroponic Controller setup complete!");
    ESP_LOGI(TAG, "========================================");
  }

  void loop() override {
    // Pump cycle logic
    if (enabled_) {
      const uint32_t now = millis();
      const uint32_t period_ms = (running_on_ ? on_minutes_ : off_minutes_) * 60000UL;
      if (now - last_change_ms_ >= period_ms) {
        running_on_ = !running_on_;
        last_change_ms_ = now;
        if (running_on_) {
          if (pump_ != nullptr) {
            pump_->turn_on().perform();
            ESP_LOGI(TAG, "Pump ON (scheduled cycle)");
          }
        } else {
          if (pump_ != nullptr) {
            pump_->turn_off().perform();
            ESP_LOGI(TAG, "Pump OFF (scheduled cycle)");
          }
        }
      }
    }

    // Light schedule logic
    if (light_sched_enabled_ && clock_ != nullptr) {
      auto time = clock_->now();
      if (time.is_valid()) {
        int current_minutes = time.hour * 60 + time.minute;
        bool should_be_on = false;
        
        if (light_on_minutes_ < light_off_minutes_) {
          should_be_on = (current_minutes >= light_on_minutes_ && current_minutes < light_off_minutes_);
        } else {
          should_be_on = (current_minutes >= light_on_minutes_ || current_minutes < light_off_minutes_);
        }
        
        if (light_ != nullptr) {
          bool is_on = light_->current_values.is_on();
          if (should_be_on && !is_on) {
            light_->turn_on().perform();
            ESP_LOGI(TAG, "Lighting ON (scheduled)");
          } else if (!should_be_on && is_on) {
            light_->turn_off().perform();
            ESP_LOGI(TAG, "Lighting OFF (scheduled)");
          }
        }
      }
    }
  }

  void dump_config() override {
    ESP_LOGCONFIG(TAG, "Hydroponic Controller:");
    ESP_LOGCONFIG(TAG, "  NVS Status: %s", settings_loaded_from_nvs_ ? "✓ Loaded from flash" : "✗ Using defaults");
    ESP_LOGCONFIG(TAG, "  Pump ON: %d min", on_minutes_);
    ESP_LOGCONFIG(TAG, "  Pump OFF: %d min", off_minutes_);
    ESP_LOGCONFIG(TAG, "  Pump Schedule: %s", enabled_ ? "enabled" : "disabled");
    ESP_LOGCONFIG(TAG, "  Light ON: %02d:%02d", light_on_minutes_/60, light_on_minutes_%60);
    ESP_LOGCONFIG(TAG, "  Light OFF: %02d:%02d", light_off_minutes_/60, light_off_minutes_%60);
    ESP_LOGCONFIG(TAG, "  Light Schedule: %s", light_sched_enabled_ ? "enabled" : "disabled");
  }

 protected:
  void start_cycle_() {
    last_change_ms_ = millis();
    running_on_ = true;
    if (pump_ != nullptr) {
      pump_->turn_on().perform();
      ESP_LOGI(TAG, "Pump cycle started");
    }
  }

  void stop_cycle_() {
    if (pump_ != nullptr) {
      pump_->turn_off().perform();
    }
    running_on_ = false;
    ESP_LOGI(TAG, "Pump cycle stopped");
  }
  
  void save_settings_() {
    ESP_LOGI(TAG, ">>> save_settings_() called <<<");
    Settings s;
    s.enabled = enabled_;
    s.on_minutes = on_minutes_;
    s.off_minutes = off_minutes_;
    s.light_sched_enabled = light_sched_enabled_;
    s.light_on_minutes = light_on_minutes_;
    s.light_off_minutes = light_off_minutes_;
    s.crc = s.calculate_crc();
    
    ESP_LOGI(TAG, "Saving: enabled=%d, on=%d, off=%d, light_sched=%d", 
             s.enabled, s.on_minutes, s.off_minutes, s.light_sched_enabled);
    
    if (pref_.save(&s)) {
      ESP_LOGI(TAG, "✓ Settings saved to flash successfully!");
    } else {
      ESP_LOGE(TAG, "✗ FAILED to save settings to flash!");
    }
  }

  void register_routes_();
  std::string build_index_() const;

  fan::Fan *pump_{nullptr};
  light::LightState *light_{nullptr};
  time::RealTimeClock *clock_{nullptr};
  web_server::WebServer *server_{nullptr};

  bool enabled_{false};
  bool running_on_{false};
  uint32_t last_change_ms_{0};
  int on_minutes_{5};
  int off_minutes_{15};
  
  bool light_sched_enabled_{false};
  int light_on_minutes_{1080};   // 18:00
  int light_off_minutes_{540};   // 09:00
  
  ESPPreferenceObject pref_;

  friend class Handler;
};

// Web Handler
class Handler : public AsyncWebHandler {
 public:
  Handler(HydroponicController *owner) : owner_(owner) {}
  
  bool canHandle(AsyncWebServerRequest *req) const override {
    auto url = req->url();
    return url == "/" || url == "/pump-cycle" || url == "/api/state" || 
           url == "/api/pump" || url == "/api/pump-cycle" || 
           url == "/api/light" || url == "/api/light-schedule";
  }
  
  void handleRequest(AsyncWebServerRequest *req) override;

 protected:
  HydroponicController *owner_;
};

// Implementations
inline void HydroponicController::register_routes_() {
  if (web_server_base::global_web_server_base == nullptr) {
    ESP_LOGE(TAG, "Web Server Base not initialized!");
    return;
  }
  
  auto *handler = new Handler(this);
  web_server_base::global_web_server_base->add_handler(handler);
  ESP_LOGD(TAG, "Web routes registered");
}

inline std::string HydroponicController::build_index_() const {
  return std::string(
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Hydroponic Tower</title>"
    "<style>"
    ":root{--bg:#0f1115;--fg:#e7e7e7;--card:#171a21;--accent:#4da3ff}"
    "body{font-family:system-ui,Segoe UI,Roboto,Arial;background:var(--bg);color:var(--fg);margin:0;padding:16px}"
    "h1{margin:0 0 16px 0;font-size:22px}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px}"
    ".card{background:var(--card);border-radius:10px;padding:16px;box-shadow:0 2px 8px rgba(0,0,0,.3)}"
    ".row{display:flex;align-items:center;gap:10px;margin:10px 0}"
    ".switch{position:relative;display:inline-block;width:48px;height:26px}"
    ".switch input{display:none}"
    ".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#3a4252;border-radius:26px;transition:.2s}"
    ".slider:before{position:absolute;content:'';height:22px;width:22px;left:2px;top:2px;background:#fff;border-radius:50%;transition:.2s}"
    "input:checked+.slider{background:var(--accent)}"
    "input:checked+.slider:before{transform:translateX(22px)}"
    "input[type=number],input[type=time]{background:#0c0e13;color:var(--fg);border:1px solid #2a2f3a;border-radius:6px;padding:6px 8px}"
    "input[type=range],progress{width:200px}"
    "progress{height:8px;border-radius:4px;background:#2a2f3a}"
    "progress::-webkit-progress-bar{background:#2a2f3a;border-radius:4px}"
    "progress::-webkit-progress-value{background:var(--accent);border-radius:4px}"
    "input[type=file]{font-size:14px;color:var(--fg)}"
    "button{background:var(--accent);color:#001c38;border:0;border-radius:6px;padding:8px 12px;cursor:pointer;font-weight:500}"
    "button:hover{opacity:0.9}"
    "#log{background:#0c0e13;border:1px solid #2a2f3a;border-radius:8px;padding:8px;height:280px;overflow:auto;white-space:pre-wrap}"
    "small.mono{font-family:ui-monospace,monospace;color:#9aa4b2}"
    ".build{position:absolute;top:20px;right:20px;font-size:11px;color:#666}"
    "</style></head><body>"
    "<h1>Hydroponic Tower</h1>"
    "<div class='grid'>"
    "  <div class='card'>"
    "    <h3>Pump</h3>"
    "    <div class='row'><label class='switch'><input id='pump_on' type='checkbox'><span class='slider'></span></label><span>Pump ON</span></div>"
    "    <div class='row'><span>Speed</span> <input id='pump_speed' type='range' min='0' max='100'><span id='pump_speed_v'>0%</span></div>"
    "    <div class='row'><label class='switch'><input id='pump_sched_en' type='checkbox'><span class='slider'></span></label><span>Schedule</span></div>"
    "    <div class='row'><label>ON, min <input id='pump_on_min' type='number' min='1' max='120' style='width:90px'></label>"
    "         <label>OFF, min <input id='pump_off_min' type='number' min='1' max='120' style='width:90px'></label>"
    "         <button id='pump_save'>Save</button><small id='pump_status' class='mono'></small></div>"
    "  </div>"
    "  <div class='card'>"
    "    <h3>Lighting</h3>"
    "    <div class='row'><label class='switch'><input id='light_on' type='checkbox'><span class='slider'></span></label><span>Light ON</span></div>"
    "    <div class='row'><span>Brightness</span> <input id='light_bri' type='range' min='0' max='100'><span id='light_bri_v'>0%</span></div>"
    "    <div class='row'><label class='switch'><input id='light_sched_en' type='checkbox'><span class='slider'></span></label><span>Schedule</span></div>"
    "    <div class='row'><label>ON <input id='light_on_time' type='time' value='18:00'></label>"
    "         <label>OFF <input id='light_off_time' type='time' value='09:00'></label>"
    "         <button id='light_save'>Save</button><small id='light_status' class='mono'></small></div>"
    "  </div>"
    "</div>"
    "<div class='card' style='margin-top:16px'>"
    "  <h3>Firmware Update</h3>"
    "  <div class='row'><input type='file' id='ota_file' accept='.bin'><button id='ota_btn'>Upload</button></div>"
    "  <div class='row'><progress id='ota_progress' style='width:100%;display:none'></progress></div>"
    "  <div class='row'><small id='ota_status' class='mono'></small></div>"
    "</div>"
    "<h3 style='margin:16px 0 8px'>Events</h3>"
    "<div id='log'></div>"
    "<script>"
    "let prev=null,ignoreNext={}; function t(){return new Date().toLocaleTimeString()}"
    "function toMin(hm){const[a,b]=hm.split(':');return (+a)*60+(+b)}"
    "function toHM(m){m=(m+1440)%1440;const h=('0'+Math.floor(m/60)).slice(-2);const mi=('0'+(m%60)).slice(-2);return h+':'+mi}"
    "function log(msg){const el=document.getElementById('log'); el.textContent+='['+t()+'] '+msg+'\\n'; el.scrollTop=el.scrollHeight}"
    "async function load(){try{const s=await fetch('/api/state').then(r=>r.json()); apply(s,true); prev=s; log('State loaded');}catch(e){log('Load error: '+e.message)}}"
    "function apply(j,full){pump_on.checked=j.pump_on; pump_speed.value=j.pump_speed; pump_speed_v.textContent=j.pump_speed+'%';"
    "if(full){pump_sched_en.checked=j.pump_sched.enabled; pump_on_min.value=j.pump_sched.on; pump_off_min.value=j.pump_sched.off;}"
    "light_on.checked=j.light_on; light_bri.value=j.light_brightness; light_bri_v.textContent=j.light_brightness+'%';"
    "if(full){light_sched_en.checked=j.light_sched.enabled; light_on_time.value=toHM(j.light_sched.on); light_off_time.value=toHM(j.light_sched.off);}}"
    "async function post(u){const r=await fetch(u,{method:'POST'}); if(!r.ok) throw new Error('HTTP');}"
    "pump_on.onchange=async()=>{try{await post('/api/pump?on='+(pump_on.checked?1:0));log('Pump '+(pump_on.checked?'ON':'OFF'));ignoreNext.pump_on=true;}catch(e){log('Pump toggle error')}};"
    "pump_speed.oninput=()=>{pump_speed_v.textContent=pump_speed.value+'%'};"
    "pump_speed.onchange=async()=>{try{await post('/api/pump?speed='+pump_speed.value);log('Pump speed '+pump_speed.value+'%');ignoreNext.pump_speed=true;}catch(e){log('Pump speed error')}};"
    "pump_sched_en.onchange=async()=>{try{const p=new URLSearchParams({enabled:pump_sched_en.checked?1:0,on:pump_on_min.value,off:pump_off_min.value});await post('/api/pump-cycle?'+p.toString());log('Pump schedule '+(pump_sched_en.checked?'enabled':'disabled'));}catch(e){log('Pump schedule toggle error');pump_sched_en.checked=!pump_sched_en.checked}};"
    "document.getElementById('pump_save').onclick=async()=>{const p=new URLSearchParams({enabled:pump_sched_en.checked?1:0,on:pump_on_min.value,off:pump_off_min.value});"
    " try{await post('/api/pump-cycle?'+p.toString());pump_status.textContent='Saved';setTimeout(()=>pump_status.textContent='',1200);log('Pump schedule times saved');}catch(e){pump_status.textContent='Error';log('Pump save error')}};"
    "light_on.onchange=async()=>{try{await post('/api/light?on='+(light_on.checked?1:0));log('Light '+(light_on.checked?'ON':'OFF'));ignoreNext.light_on=true;}catch(e){log('Light toggle error')}};"
    "light_bri.oninput=()=>{light_bri_v.textContent=light_bri.value+'%'};"
    "light_bri.onchange=async()=>{try{await post('/api/light?brightness='+light_bri.value);log('Light brightness '+light_bri.value+'%');ignoreNext.light_bri=true;}catch(e){log('Light brightness error')}};"
    "light_sched_en.onchange=async()=>{try{const on=toMin(light_on_time.value),off=toMin(light_off_time.value);const p=new URLSearchParams({enabled:light_sched_en.checked?1:0,on:on,off:off});await post('/api/light-schedule?'+p.toString());log('Light schedule '+(light_sched_en.checked?'enabled':'disabled'));}catch(e){log('Light schedule toggle error');light_sched_en.checked=!light_sched_en.checked}};"
    "document.getElementById('light_save').onclick=async()=>{const on=toMin(light_on_time.value),off=toMin(light_off_time.value);const p=new URLSearchParams({enabled:light_sched_en.checked?1:0,on:on,off:off});"
    " try{await post('/api/light-schedule?'+p.toString());light_status.textContent='Saved';setTimeout(()=>light_status.textContent='',1200);log('Light schedule times saved');}catch(e){light_status.textContent='Error';log('Light save error')}};"
    "document.getElementById('ota_btn').onclick=async()=>{"
    " const file=ota_file.files[0]; if(!file){ota_status.textContent='Select file first';return;}"
    " const formData=new FormData(); formData.append('file',file);"
    " ota_status.textContent='Uploading...'; ota_progress.style.display='block'; ota_progress.value=0;"
    " try{"
    "  const xhr=new XMLHttpRequest();"
    "  xhr.upload.onprogress=e=>{if(e.lengthComputable)ota_progress.value=(e.loaded/e.total)*100;};"
    "  xhr.onload=()=>{if(xhr.status===200){ota_status.textContent='✓ Success! Rebooting...';log('Firmware uploaded, rebooting...');}else{ota_status.textContent='✗ Upload failed';}};"
    "  xhr.onerror=()=>{ota_status.textContent='✗ Network error';};"
    "  xhr.open('POST','/update'); xhr.send(formData);"
    " }catch(e){ota_status.textContent='✗ Error: '+e.message;}"
    "};"
    "setInterval(async()=>{try{const s=await fetch('/api/state').then(r=>r.json()); if(prev){if(prev.pump_on!==s.pump_on && !ignoreNext.pump_on) log('Pump '+(s.pump_on?'ON':'OFF')); if(prev.pump_speed!==s.pump_speed && !ignoreNext.pump_speed) log('Pump speed '+s.pump_speed+'%'); if(prev.light_on!==s.light_on && !ignoreNext.light_on) log('Light '+(s.light_on?'ON':'OFF')); if(prev.light_brightness!==s.light_brightness && !ignoreNext.light_bri) log('Light brightness '+s.light_brightness+'%');} ignoreNext={}; prev=s; apply(s,false);}catch(e){log('Poll error')}} ,2000);"
    "log('UI loaded'); load();"
    "</script>"
    "</body></html>");
}

inline void Handler::handleRequest(AsyncWebServerRequest *req) {
  using namespace web_server_idf;
  auto url = req->url();
  
  // Root page
  if (url == "/" || url == "/pump-cycle") {
    std::string html = owner_->build_index_();
    req->send(200, "text/html", html.c_str());
    return;
  }
  
  // State API
  if (url == "/api/state") {
    int pump_speed = 0;
    bool pump_on = false;
    if (owner_->pump_ != nullptr) {
      pump_on = owner_->pump_->state;
      pump_speed = owner_->pump_->speed;
    }
    
    int light_brightness = 0;
    bool light_on = false;
    if (owner_->light_ != nullptr) {
      light_on = owner_->light_->current_values.is_on();
      light_brightness = (int)(owner_->light_->current_values.get_brightness() * 100.0f + 0.5f);
    }
    
    char buf[256];
    snprintf(buf, sizeof(buf),
      "{\"pump_on\":%s,\"pump_speed\":%d,\"pump_sched\":{\"enabled\":%s,\"on\":%d,\"off\":%d},\"light_on\":%s,\"light_brightness\":%d,\"light_sched\":{\"enabled\":%s,\"on\":%d,\"off\":%d}}",
      pump_on?"true":"false", pump_speed, owner_->enabled_?"true":"false", owner_->on_minutes_, owner_->off_minutes_,
      light_on?"true":"false", light_brightness, owner_->light_sched_enabled_?"true":"false", owner_->light_on_minutes_, owner_->light_off_minutes_);
    req->send(200, "application/json", buf);
    return;
  }
  
  // Pump control API
  if (url == "/api/pump" && req->method() == HTTP_POST) {
    if (req->hasParam("on")) {
      bool on = req->getParam("on")->value() == "1";
      if (owner_->pump_ != nullptr) {
        if (on) owner_->pump_->turn_on().perform(); 
        else owner_->pump_->turn_off().perform();
      }
    }
    if (req->hasParam("speed")) {
      int speed = atoi(req->getParam("speed")->value().c_str());
      if (owner_->pump_ != nullptr) {
        auto call = owner_->pump_->make_call();
        call.set_speed(speed);
        call.perform();
      }
    }
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  
  // Pump schedule API
  if (url == "/api/pump-cycle" && req->method() == HTTP_POST) {
    bool changed = false;
    if (req->hasParam("enabled")) {
      bool enabled = req->getParam("enabled")->value() == "1";
      owner_->enabled_ = enabled;
      if (enabled) owner_->start_cycle_(); 
      else owner_->stop_cycle_();
      changed = true;
    }
    if (req->hasParam("on")) {
      owner_->on_minutes_ = std::max(1, std::min(120, atoi(req->getParam("on")->value().c_str())));
      changed = true;
    }
    if (req->hasParam("off")) {
      owner_->off_minutes_ = std::max(1, std::min(120, atoi(req->getParam("off")->value().c_str())));
      changed = true;
    }
    if (changed) {
      owner_->save_settings_();
    }
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  
  // Light control API
  if (url == "/api/light" && req->method() == HTTP_POST) {
    if (req->hasParam("on")) {
      bool on = req->getParam("on")->value() == "1";
      if (owner_->light_ != nullptr) {
        if (on) owner_->light_->turn_on().perform();
        else owner_->light_->turn_off().perform();
      }
    }
    if (req->hasParam("brightness")) {
      float brightness = atof(req->getParam("brightness")->value().c_str()) / 100.0f;
      if (owner_->light_ != nullptr) {
        auto call = owner_->light_->make_call();
        call.set_brightness(brightness);
        call.perform();
      }
    }
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  
  // Light schedule API
  if (url == "/api/light-schedule" && req->method() == HTTP_POST) {
    bool changed = false;
    if (req->hasParam("enabled")) {
      owner_->light_sched_enabled_ = req->getParam("enabled")->value() == "1";
      changed = true;
    }
    if (req->hasParam("on")) {
      owner_->light_on_minutes_ = std::max(0, std::min(1439, atoi(req->getParam("on")->value().c_str())));
      changed = true;
    }
    if (req->hasParam("off")) {
      owner_->light_off_minutes_ = std::max(0, std::min(1439, atoi(req->getParam("off")->value().c_str())));
      changed = true;
    }
    if (changed) {
      owner_->save_settings_();
    }
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  
  req->send(404);
}

}  // namespace hydroponic_controller
}  // namespace esphome
