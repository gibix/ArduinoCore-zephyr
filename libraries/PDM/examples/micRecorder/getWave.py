import serial
import wave
import struct
import time

# --- CONFIGURATION ---
SERIAL_PORT = '/dev/ttyACM0'  # CHECK YOUR ARDUINO IDE FOR THE CORRECT PORT!
BAUD_RATE = 115200
OUTPUT_FILENAME = "recording.wav"
RECORD_SECONDS = 5     # How long to record
SAMPLE_RATE = 16000    # Must match Arduino sketch
# ---------------------

def record_audio():
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE)
        print(f"Connected to {SERIAL_PORT}. Recording for {RECORD_SECONDS} seconds...")
        
        # Clear buffer to avoid old data
        ser.reset_input_buffer()
        
        audio_frames = []
        start_time = time.time()
        
        # Calculate how many bytes we expect
        # 16000 samples/sec * 2 bytes/sample * seconds
        total_bytes = SAMPLE_RATE * 2 * RECORD_SECONDS
        bytes_received = 0
        
        while bytes_received < total_bytes:
            if ser.in_waiting > 0:
                # Read whatever is available
                data = ser.read(ser.in_waiting)
                audio_frames.append(data)
                bytes_received += len(data)
                
                # Simple progress indicator
                print(f"Recorded: {bytes_received} / {total_bytes} bytes", end='\r')

        print("\nRecording complete. Saving...")
        ser.close()

        # Save to WAV file
        with wave.open(OUTPUT_FILENAME, 'wb') as wf:
            wf.setnchannels(1)      # Mono
            wf.setsampwidth(2)      # 16-bit (2 bytes)
            wf.setframerate(SAMPLE_RATE)
            wf.writeframes(b''.join(audio_frames))
            
        print(f"File saved: {OUTPUT_FILENAME}")
        
    except serial.SerialException:
        print(f"Error: Could not open port {SERIAL_PORT}. Is the Arduino Monitor open? Close it!")
    except Exception as e:
        print(f"An error occurred: {e}")

if __name__ == "__main__":
    record_audio()
