# Real-Time Audio Conferencing Server

A C++ media server that receives audio from multiple participants, synchronizes streams using GCC-PHAT, mixes them, and delivers synchronized audio to all listeners.

## Requirements

- Ubuntu/Debian (Windows WSL2 works) or macOS
- C++17 compiler
- CMake 3.14+
- Internet connection (for downloading dependencies)

## Installation
### 1. Install Dependencies
**Ubuntu/Debian/WSL2:**
```bash
sudo apt update
sudo apt install -y build-essential cmake libboost-system-dev libboost-thread-dev pkg-config
```
## Clone and build 
```
git clone 'Link'
cd Real-time-Audio-Gcc-phat-Implementation
mkdir build && cd build
cmake ..
make -j$(nproc)
```
## RUN
```
cd build
sudo ./speeqr_server
```
## Connect
Open browser to: http://localhost:8080

## Usage
Click "Join Room" (room name defaults to "Room_A") then allow microphone access

Open another browser tab to the same URL and join the same room

Speak into one tab - you'll hear audio in the other

## Project Structure
```
src/
├── main.cpp              # Entry point, starts all services
├── ntp_server.cpp/h      # NTP time server (UDP port 123)
├── http_handler.cpp/h    # Serves web client and /time endpoint
├── websocket_handler.cpp/h # WebSocket connections, rooms, mixer thread
├── audio_buffer.cpp/h    # Jitter buffer, VAD, audio mixing
└── gcc_phat.cpp/h        # GCC-PHAT delay estimation (FFT-based)

