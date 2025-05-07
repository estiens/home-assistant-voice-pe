// voice_assistant.cpp

#include "voice_assistant.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <ArduinoJson.h>
#include <cinttypes>

namespace esphome {
namespace voice_assistant {

static const char *const TAG = "voice_assistant";

// Audio configuration
static const size_t SAMPLE_RATE_HZ = 16000;
static const size_t INPUT_BUFFER_SIZE = 32 * SAMPLE_RATE_HZ / 1000;  // 32ms * 16kHz / 1000ms
static const size_t BUFFER_SIZE = 512 * SAMPLE_RATE_HZ / 1000;
static const size_t SEND_BUFFER_SIZE = INPUT_BUFFER_SIZE * sizeof(int16_t);
static const size_t RECEIVE_SIZE = 1024;
static const size_t SPEAKER_BUFFER_SIZE = 16 * RECEIVE_SIZE;

// Static instance for global access
VoiceAssistant *global_voice_assistant = nullptr;

VoiceAssistant::VoiceAssistant() {
  global_voice_assistant = this;
}

float VoiceAssistant::get_setup_priority() const {
  return setup_priority::AFTER_CONNECTION;
}

void VoiceAssistant::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Voice Assistant...");
  
  // Subscribe to MQTT command topic if MQTT client is available
  if (this->mqtt_client_ != nullptr) {
    this->mqtt_client_->subscribe(this->command_topic_, [this](const std::string &topic, const std::string &payload) {
      this->on_mqtt_message(topic, payload);
    });
    
    // Publish initial state
    this->publish_state();
  } else {
    ESP_LOGW(TAG, "MQTT client not set, voice assistant won't respond to MQTT commands");
  }
}

void VoiceAssistant::loop() {
  // Check for timeouts in the current state
  this->check_timeout_();
  
  // State machine
  switch (this->state_) {
    case State::IDLE: {
      if (this->continuous_ && this->desired_state_ == State::IDLE) {
        this->idle_trigger_->trigger();
        this->set_state_(State::START_MICROPHONE, State::STREAMING_MICROPHONE);
      }
      break;
    }
    
    case State::START_MICROPHONE: {
      ESP_LOGD(TAG, "Starting Microphone");
      if (!this->allocate_buffers_()) {
        ESP_LOGE(TAG, "Failed to allocate buffers");
        this->set_state_(State::IDLE);
        this->error_trigger_->trigger("buffer_error", "Failed to allocate buffers");
        this->publish_event(EVENT_ERROR, "{\"code\":\"buffer_error\",\"message\":\"Failed to allocate buffers\"}");
        break;
      }
      
      this->clear_buffers_();
      this->mic_->start();
      this->set_state_(State::STARTING_MICROPHONE);
      break;
    }
    
    case State::STARTING_MICROPHONE: {
      if (this->mic_->is_running()) {
        // Play start sound if we have a speaker
#ifdef USE_SPEAKER
        this->play_start_sound();
#endif
        this->set_state_(this->desired_state_);
        this->publish_event(EVENT_STT_START);
        this->listening_trigger_->trigger();
      }
      break;
    }
    
    case State::STREAMING_MICROPHONE: {
      // Read audio and publish to MQTT
      size_t bytes_read = this->read_microphone_();
      if (bytes_read > 0 && this->mqtt_client_ != nullptr) {
        size_t available = this->ring_buffer_->available();
        
        // Process in chunks to avoid overflowing MQTT buffer
        const size_t chunk_size = 1024;  // 1KB chunks
        while (available >= chunk_size) {
          std::vector<uint8_t> audio_data(chunk_size);
          size_t read_bytes = this->ring_buffer_->read(audio_data.data(), chunk_size, 0);
          
          // Apply gain if needed
          if (this->audio_gain_ != 1.0f && read_bytes > 0) {
            int16_t *samples = reinterpret_cast<int16_t *>(audio_data.data());
            size_t num_samples = read_bytes / sizeof(int16_t);
            
            for (size_t i = 0; i < num_samples; i++) {
              int32_t sample = samples[i];
              sample = static_cast<int32_t>(sample * this->audio_gain_);
              samples[i] = static_cast<int16_t>(std::max(std::min(sample, (int32_t)32767), (int32_t)-32768));
            }
          }
          
          // Publish audio chunk to MQTT
          this->mqtt_client_->publish(this->audio_output_topic_, audio_data);
          available = this->ring_buffer_->available();
        }
      }
      break;
    }
    
    case State::STOP_MICROPHONE: {
      if (this->mic_->is_running()) {
        this->mic_->stop();
        this->set_state_(State::STOPPING_MICROPHONE);
      } else {
        this->set_state_(this->desired_state_);
      }
      break;
    }
    
    case State::STOPPING_MICROPHONE: {
      if (this->mic_->is_stopped()) {
        // Play stop sound if we have a speaker
#ifdef USE_SPEAKER
        this->play_stop_sound();
#endif
        this->set_state_(this->desired_state_);
      }
      break;
    }
    
    case State::AWAITING_RESPONSE: {
      // Just wait for response via MQTT
      break;
    }
    
    case State::STREAMING_RESPONSE: {
      // Check if playback is complete
#ifdef USE_MEDIA_PLAYER
      if (this->media_player_ != nullptr) {
        if (this->media_player_->state != media_player::MEDIA_PLAYER_STATE_PLAYING) {
          // Playback complete
          this->set_state_(State::RESPONSE_FINISHED);
          this->publish_event(EVENT_TTS_END);
          this->tts_end_trigger_->trigger("playback_complete");
        }
      }
#endif
      break;
    }
    
    case State::RESPONSE_FINISHED: {
      // Return to IDLE or start listening again if in continuous mode
      if (this->continuous_) {
        this->set_state_(State::START_MICROPHONE, State::STREAMING_MICROPHONE);
      } else {
        this->set_state_(State::IDLE);
        this->publish_event(EVENT_RUN_END);
        this->end_trigger_->trigger();
      }
      break;
    }
    
    default:
      break;
  }
}

// Handle MQTT messages
void VoiceAssistant::on_mqtt_message(const std::string &topic, const std::string &payload) {
  if (topic != this->command_topic_)
    return;
  
  // Parse JSON payload
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    ESP_LOGE(TAG, "JSON parsing error: %s", error.c_str());
    return;
  }
  
  // Handle commands
  if (doc.containsKey("action")) {
    std::string action = doc["action"].as<std::string>();
    
    if (action == "start" || action == "listen") {
      // Start listening
      bool continuous = doc.containsKey("continuous") ? doc["continuous"].as<bool>() : false;
      bool silence_detection = doc.containsKey("silence_detection") ? doc["silence_detection"].as<bool>() : true;
      this->request_start(continuous, silence_detection);
      
    } else if (action == "stop") {
      // Stop listening
      this->request_stop();
      
    } else if (action == "play") {
      // Play audio from URL
      if (doc.containsKey("url")) {
        std::string url = doc["url"].as<std::string>();
        
#ifdef USE_MEDIA_PLAYER
        if (this->media_player_ != nullptr) {
          // Set state to streaming response
          this->set_state_(State::STREAMING_RESPONSE);
          this->publish_event(EVENT_TTS_START, "{\"url\":\"" + url + "\"}");
          this->tts_start_trigger_->trigger(url);
          
          // Play URL
          this->media_player_->make_call()
            .set_media_url(url)
            .perform();
        } else {
          ESP_LOGW(TAG, "No media player available for URL playback");
          this->publish_event(EVENT_ERROR, "{\"code\":\"no_media_player\",\"message\":\"No media player available\"}");
          this->error_trigger_->trigger("no_media_player", "No media player available");
        }
#else
        ESP_LOGW(TAG, "Media player support not enabled");
        this->publish_event(EVENT_ERROR, "{\"code\":\"media_player_disabled\",\"message\":\"Media player support not enabled\"}");
        this->error_trigger_->trigger("media_player_disabled", "Media player support not enabled");
#endif
      }
    }
  }
}

// Publish state to MQTT
void VoiceAssistant::publish_state() {
  if (this->mqtt_client_ == nullptr)
    return;
  
  std::string state_str;
  switch (this->state_) {
    case State::IDLE: state_str = "IDLE"; break;
    case State::START_MICROPHONE: state_str = "START_MICROPHONE"; break;
    case State::STARTING_MICROPHONE: state_str = "STARTING_MICROPHONE"; break;
    case State::STREAMING_MICROPHONE: state_str = "LISTENING"; break;
    case State::STOP_MICROPHONE: state_str = "STOP_MICROPHONE"; break;
    case State::STOPPING_MICROPHONE: state_str = "STOPPING_MICROPHONE"; break;
    case State::AWAITING_RESPONSE: state_str = "AWAITING_RESPONSE"; break;
    case State::STREAMING_RESPONSE: state_str = "SPEAKING"; break;
    case State::RESPONSE_FINISHED: state_str = "RESPONSE_FINISHED"; break;
    default: state_str = "UNKNOWN"; break;
  }
  
  char buf[128];
  snprintf(buf, sizeof(buf), 
           "{\"state\":\"%s\",\"continuous\":%s,\"timestamp\":%u}", 
           state_str.c_str(), 
           this->continuous_ ? "true" : "false", 
           (unsigned int)millis());
  
  this->mqtt_client_->publish(this->state_topic_, buf);
}

// Publish events to MQTT
void VoiceAssistant::publish_event(EventType event_type, const std::string &data) {
  if (this->mqtt_client_ == nullptr)
    return;
  
  std::string event_name;
  switch (event_type) {
    case EVENT_RUN_START: event_name = "run_start"; break;
    case EVENT_WAKE_WORD_START: event_name = "wake_word_start"; break;
    case EVENT_WAKE_WORD_END: event_name = "wake_word_end"; break;
    case EVENT_STT_START: event_name = "stt_start"; break;
    case EVENT_STT_END: event_name = "stt_end"; break;
    case EVENT_INTENT_START: event_name = "intent_start"; break;
    case EVENT_INTENT_END: event_name = "intent_end"; break;
    case EVENT_TTS_START: event_name = "tts_start"; break;
    case EVENT_TTS_END: event_name = "tts_end"; break;
    case EVENT_RUN_END: event_name = "run_end"; break;
    case EVENT_ERROR: event_name = "error"; break;
    case EVENT_STT_VAD_START: event_name = "stt_vad_start"; break;
    case EVENT_STT_VAD_END: event_name = "stt_vad_end"; break;
    case EVENT_TTS_STREAM_START: event_name = "tts_stream_start"; break;
    case EVENT_TTS_STREAM_END: event_name = "tts_stream_end"; break;
    default: event_name = "unknown"; break;
  }
  
  if (data.empty()) {
    char buf[128];
    snprintf(buf, sizeof(buf), 
             "{\"event\":\"%s\",\"timestamp\":%u}", 
             event_name.c_str(), 
             (unsigned int)millis());
    this->mqtt_client_->publish(this->event_topic_, buf);
  } else {
    // Data already contains JSON payload, merge with event info
    char buf[512];
    snprintf(buf, sizeof(buf), 
             "{\"event\":\"%s\",\"timestamp\":%u,%s", 
             event_name.c_str(), 
             (unsigned int)millis(),
             data.c_str() + 1);  // Skip first '{' from data
    this->mqtt_client_->publish(this->event_topic_, buf);
  }
}

// Request to start listening
void VoiceAssistant::request_start(bool continuous, bool silence_detection) {
  if (this->state_ == State::IDLE) {
    this->continuous_ = continuous;
    this->silence_detection_ = silence_detection;
    this->set_state_(State::START_MICROPHONE, State::STREAMING_MICROPHONE);
    this->publish_event(EVENT_RUN_START);
    this->start_trigger_->trigger();
  }
}

// Request to stop listening
void VoiceAssistant::request_stop() {
  this->continuous_ = false;
  
  switch (this->state_) {
    case State::IDLE:
      break;
    case State::START_MICROPHONE:
    case State::STARTING_MICROPHONE:
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      break;
    case State::STREAMING_MICROPHONE:
      this->set_state_(State::STOP_MICROPHONE, State::IDLE);
      break;
    case State::STOP_MICROPHONE:
    case State::STOPPING_MICROPHONE:
      this->desired_state_ = State::IDLE;
      break;
    case State::AWAITING_RESPONSE:
      this->set_state_(State::IDLE);
      break;
    case State::STREAMING_RESPONSE:
#ifdef USE_MEDIA_PLAYER
      if (this->media_player_ != nullptr) {
        this->media_player_->make_call()
          .set_command(media_player::MEDIA_PLAYER_COMMAND_STOP)
          .perform();
      }
#endif
      this->set_state_(State::RESPONSE_FINISHED);
      break;
    case State::RESPONSE_FINISHED:
      break;
  }
}

// Check for timeouts in the current state
void VoiceAssistant::check_timeout_() {
  if (this->state_ == State::IDLE)
    return;
  
  uint32_t current_time = millis();
  uint32_t elapsed_time = current_time - this->state_timestamp_;
  
  // Check for timeout in streaming microphone state
  if (this->state_ == State::STREAMING_MICROPHONE && elapsed_time > this->timeout_duration_) {
    ESP_LOGW(TAG, "Timeout in STREAMING_MICROPHONE state after %u ms", elapsed_time);
    this->set_state_(State::STOP_MICROPHONE, State::IDLE);
    this->publish_event(EVENT_ERROR, "{\"code\":\"timeout\",\"message\":\"Listening timeout\"}");
    this->error_trigger_->trigger("timeout", "Listening timeout");
  }
  
  // Check for timeout in streaming response state
  if (this->state_ == State::STREAMING_RESPONSE && elapsed_time > this->timeout_duration_) {
    ESP_LOGW(TAG, "Timeout in STREAMING_RESPONSE state after %u ms", elapsed_time);
    this->set_state_(State::RESPONSE_FINISHED);
    this->publish_event(EVENT_ERROR, "{\"code\":\"playback_timeout\",\"message\":\"Playback timeout\"}");
    this->error_trigger_->trigger("playback_timeout", "Playback timeout");
  }
}

// Reset state if stuck
void VoiceAssistant::reset_state_() {
  this->set_state_(State::IDLE);
  this->continuous_ = false;
  this->clear_buffers_();
  
  if (this->mic_->is_running()) {
    this->mic_->stop();
  }
  
#ifdef USE_MEDIA_PLAYER
  if (this->media_player_ != nullptr) {
    this->media_player_->make_call()
      .set_command(media_player::MEDIA_PLAYER_COMMAND_STOP)
      .perform();
  }
#endif
  
  this->publish_state();
  this->publish_event(EVENT_ERROR, "{\"code\":\"reset\",\"message\":\"State machine reset\"}");
  this->error_trigger_->trigger("reset", "State machine reset");
}

// Update state with timestamp and publish
void VoiceAssistant::set_state_(State state) {
  State old_state = this->state_;
  this->state_ = state;
  this->state_timestamp_ = millis();
  
  ESP_LOGD(TAG, "State changed from %d to %d", (int)old_state, (int)state);
  this->publish_state();
}

// Update state and desired state with timestamp and publish
void VoiceAssistant::set_state_(State state, State desired_state) {
  this->set_state_(state);
  this->desired_state_ = desired_state;
  ESP_LOGD(TAG, "Desired state set to %d", (int)desired_state);
}

// Allocate audio buffers
bool VoiceAssistant::allocate_buffers_() {
  if (this->send_buffer_ != nullptr) {
    return true;  // Already allocated
  }

#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    ExternalRAMAllocator<uint8_t> speaker_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    this->speaker_buffer_ = speaker_allocator.allocate(SPEAKER_BUFFER_SIZE);
    if (this->speaker_buffer_ == nullptr) {
      ESP_LOGW(TAG, "Could not allocate speaker buffer");
      return false;
    }
  }
#endif

  ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  this->input_buffer_ = allocator.allocate(INPUT_BUFFER_SIZE);
  if (this->input_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate input buffer");
    return false;
  }

#ifdef USE_ESP_ADF
  this->vad_instance_ = vad_create(VAD_MODE_4);
#endif

  this->ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate ring buffer");
    return false;
  }

  ExternalRAMAllocator<uint8_t> send_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->send_buffer_ = send_allocator.allocate(SEND_BUFFER_SIZE);
  if (send_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate send buffer");
    return false;
  }

  return true;
}

// Clear audio buffers
void VoiceAssistant::clear_buffers_() {
  if (this->send_buffer_ != nullptr) {
    memset(this->send_buffer_, 0, SEND_BUFFER_SIZE);
  }

  if (this->input_buffer_ != nullptr) {
    memset(this->input_buffer_, 0, INPUT_BUFFER_SIZE * sizeof(int16_t));
  }

  if (this->ring_buffer_ != nullptr) {
    this->ring_buffer_->reset();
  }

#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    if (this->speaker_buffer_ != nullptr) {
      memset(this->speaker_buffer_, 0, SPEAKER_BUFFER_SIZE);
      this->speaker_buffer_size_ = 0;
      this->speaker_buffer_index_ = 0;
    }
  }
#endif
}

// Deallocate audio buffers
void VoiceAssistant::deallocate_buffers_() {
  ExternalRAMAllocator<uint8_t> send_deallocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  send_deallocator.deallocate(this->send_buffer_, SEND_BUFFER_SIZE);
  this->send_buffer_ = nullptr;

  if (this->ring_buffer_ != nullptr) {
    this->ring_buffer_.reset();
    this->ring_buffer_ = nullptr;
  }

#ifdef USE_ESP_ADF
  if (this->vad_instance_ != nullptr) {
    vad_destroy(this->vad_instance_);
    this->vad_instance_ = nullptr;
  }
#endif

  ExternalRAMAllocator<int16_t> input_deallocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  input_deallocator.deallocate(this->input_buffer_, INPUT_BUFFER_SIZE);
  this->input_buffer_ = nullptr;

#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    if (this->speaker_buffer_ != nullptr) {
      ExternalRAMAllocator<uint8_t> speaker_deallocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
      speaker_deallocator.deallocate(this->speaker_buffer_, SPEAKER_BUFFER_SIZE);
      this->speaker_buffer_ = nullptr;
    }
  }
#endif
}

// Read audio from microphone
int VoiceAssistant::read_microphone_() {
  size_t bytes_read = 0;
  if (this->mic_->is_running()) {
    bytes_read = this->mic_->read(this->input_buffer_, INPUT_BUFFER_SIZE * sizeof(int16_t));
    if (bytes_read == 0) {
      memset(this->input_buffer_, 0, INPUT_BUFFER_SIZE * sizeof(int16_t));
      return 0;
    }
    
    // Write audio into ring buffer
    this->ring_buffer_->write((void *) this->input_buffer_, bytes_read);
  } else {
    ESP_LOGD(TAG, "Microphone not running");
  }
  return bytes_read;
}

// Play start sound effect
void VoiceAssistant::play_start_sound() {
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    // Simple beep tone - this is where you'd play a start chime
    // For a real implementation, you'd include the audio data
    static const uint8_t beep_tone[] = {
      // Simple beep sound data would go here
      // For now, we'll just create a simple tone
      0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 
      0xFF, 0xF0, 0xE0, 0xD0, 0xC0, 0xB0, 0xA0, 0x90
    };
    
    // Generate a simple tone
    std::vector<uint8_t> tone_data;
    for (int i = 0; i < 4000; i++) {
      tone_data.push_back(beep_tone[i % sizeof(beep_tone)]);
    }
    
    this->speaker_->play(tone_data.data(), tone_data.size());
  }
#endif
}

// Play stop sound effect
void VoiceAssistant::play_stop_sound() {
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    // Simple end beep tone - this is where you'd play a stop chime
    static const uint8_t end_beep[] = {
      // Simple end beep sound data would go here
      0xF0, 0xE0, 0xD0, 0xC0, 0xB0, 0xA0, 0x90, 0x80,
      0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10, 0x00
    };
    
    // Generate a simple tone
    std::vector<uint8_t> tone_data;
    for (int i = 0; i < 4000; i++) {
      tone_data.push_back(end_beep[i % sizeof(end_beep)]);
    }
    
    this->speaker_->play(tone_data.data(), tone_data.size());
  }
#endif
}

// Play error sound effect
void VoiceAssistant::play_error_sound() {
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    // Simple error tone - this is where you'd play an error sound
    static const uint8_t error_tone[] = {
      // Simple error sound data would go here
      0x80, 0x40, 0x80, 0xC0, 0x80, 0x40, 0x80, 0xC0
    };
    
    // Generate a simple tone
    std::vector<uint8_t> tone_data;
    for (int i = 0; i < 4000; i++) {
      tone_data.push_back(error_tone[i % sizeof(error_tone)]);
    }
    
    this->speaker_->play(tone_data.data(), tone_data.size());
  }
#endif
}

}  // namespace voice_assistant
}  // namespace esphome