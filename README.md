# Hydroponic Tower Controller

ESPHome custom component for automated hydroponic tower management with pump cycling and lighting control.

## Features

- **Pump Cycle Control**: Automatic ON/OFF cycling with configurable intervals
- **Light Scheduling**: Time-based lighting control with brightness adjustment
- **Web Interface**: Modern, responsive web UI for manual control and monitoring
- **Real-time State**: Live updates of pump and light status
- **REST API**: Full API for integration with Home Assistant or other systems

## Installation

Add this to your ESPHome YAML configuration:

```yaml
external_components:
  - source: github://chymaslik/hydroponic-tower@main
    components: [ hydroponic_controller ]

hydroponic_controller:
  pump_id: water_pump
  light_id: lighting
  time_id: sntp_time
  web_server_id: web_srv
  on_minutes: 5      # Pump ON duration
  off_minutes: 15    # Pump OFF duration
  enabled: false     # Start with schedule disabled
```

## Configuration Variables

- **pump_id** (*Required*, ID): ID of the fan component controlling the pump
- **light_id** (*Required*, ID): ID of the light component
- **time_id** (*Required*, ID): ID of the time component for scheduling
- **web_server_id** (*Required*, ID): ID of the web_server component
- **on_minutes** (*Optional*, int): Pump ON duration in minutes (1-120, default: 5)
- **off_minutes** (*Optional*, int): Pump OFF duration in minutes (1-120, default: 15)
- **enabled** (*Optional*, boolean): Enable pump schedule on startup (default: false)

## Hardware Requirements

- ESP32-C3 (or any ESP32 variant)
- MOSFET modules for pump and lighting control
- Water pump
- LED grow lights
- 12V power supply

## Web Interface

Access the web interface at `http://[device-ip]/` to:
- Manually control pump and lighting
- Configure pump cycle intervals
- Set light schedule (ON/OFF times)
- View real-time events log

## API Endpoints

- `GET /api/state` - Get current state
- `POST /api/pump?on=[0|1]&speed=[0-100]` - Control pump
- `POST /api/pump-cycle?enabled=[0|1]&on=[minutes]&off=[minutes]` - Configure pump schedule
- `POST /api/light?on=[0|1]&brightness=[0-100]` - Control lighting
- `POST /api/light-schedule?enabled=[0|1]&on=[minutes]&off=[minutes]` - Configure light schedule

## Example Configuration

See [hydroponics.yaml](hydroponics.yaml) for a complete working example.

## License

MIT License

