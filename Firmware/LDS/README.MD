# Local Data Storage

## Use case:
To provide more storagespace for Thingy91x in order to store readings for when field tested.
<br>
Data that will be stored is mainly:
- PPK data for an overview of power consumption
- Location data/sensor data

## Function



### Data Flow

```mermaid
flowchart LR
    A["Thingy91X"] <-->|Sensor data| B["ESP32"]
    A -->|Powe use| D["PPK"]
    B <-->|Power use <br>Sensor data| C["SD Card"]
	D --> |Power use|B
```


### Protocols
```mermaid
flowchart LR
    A["Thingy91X"] <-->|I2C| B["ESP32"]
    D["PPK"]
    B <-->|SPI| C["SD Card"]
	D --> |CDC|B
```



## Files
- Main.cpp: main run file 
- Config.h is for defining constant variables for ease of change
- sdcard.h: sdcard read/write function
- ppk_reader.h: interprate ppk data