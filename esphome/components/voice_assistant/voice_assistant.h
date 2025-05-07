// voice_assistant.h - Core modifications

#pragma once

#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "esphome/components/microphone/microphone.h"
#ifdef USE_MICRO_WAKE_WORD
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#endif
#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif
#ifdef USE_MEDIA_PLAYER
#include "esphome/components/media_player/media_player.h"
#endif
#include "esphome/components/mqtt/mqtt_client.h"

#ifdef USE_ESP_ADF
#include <esp_vad.h>
#endif

namespace esphome {
namespace voice_assistant {

// Define state enum for the voice assistant
enum class State {
  IDLE,
  START_MICROPHONE,
  STARTING_MICROPHONE,
  WAIT_FOR_VAD,
  WAITING_FOR_VAD,
  STREAMING_MICROPHONE,
  STOP_MICROPHONE,
  STOPPING_MICROPHONE,
  AWAITING_RESPONSE,
  STREAMING_RESPONSE,
  RESPONSE_FINISHED,
};

// Define event types for MQTT events
enum EventType {
  EVENT_RUN_START = 0,
  EVENT_WAKE_WORD_START = 1,
  EVENT_WAKE_WORD_END = 2,
  EVENT_STT_START = 3,
  EVENT_STT_END = 4,
  EVENT_INTENT_START = 5,
  EVENT_INTENT_END = 6,
  EVENT_TTS_START = 7,
  EVENT_TTS_END = 8,
  EVENT_RUN_END = 9,
  EVENT_ERROR = 10,
  EVENT_STT_VAD_START = 11,
  EVENT_STT_VAD_END = 12,
  EVENT_TTS_STREAM_START = 13,
  EVENT_TTS_STREAM_END = 14,
};

// Audio mode enum
enum AudioMode : uint8_t {
  AUDIO_MODE_HTTP,
  AUDIO_MODE_MQTT,
};

class VoiceAssistant : public Component {
 public:
  VoiceAssistant();
  void setup() override;
  void loop() override;
  float get_setup_priority() const override;

  // MQTT client setter
  void set_mqtt_client(mqtt::MQTTClientComponent *mqtt_client) { this->mqtt_client_ = mqtt_client; }
  
  // Setters for components
  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }
#ifdef USE_MICRO_WAKE_WORD
  void set_micro_wake_word(micro_wake_word::MicroWakeWord *mww) { this->micro_wake_word_ = mww; }
#endif
#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *speaker) {
    this->speaker_ = speaker;
    this->local_output_ = true;
  }
#endif
#ifdef USE_MEDIA_PLAYER
  void set_media_player(media_player::MediaPlayer *media_player) {
    this->media_player_ = media_player;
    this->local_output_ = true;
  }
#endif

  // Control methods
  void request_start(bool continuous, bool silence_detection);
  void request_stop();
  bool is_running() const { return this->state_ != State::IDLE; }
  
  // MQTT topic configuration
  void set_audio_output_topic(const std::string &topic) { this->audio_output_topic_ = topic; }
  void set_state_topic(const std::string &topic) { this->state_topic_ = topic; }
  void set_command_topic(const std::string &topic) { this->command_topic_ = topic; }
  void set_event_topic(const std::string &topic) { this->event_topic_ = topic; }
  
  // Event handling
  void on_mqtt_message(const std::string &topic, const std::string &payload);
  void publish_event(EventType event_type, const std::string &data = "");
  void publish_state();

  // Audio streams
  void set_audio_gain(float gain) { this->audio_gain_ = gain; }
  void set_silence_detection(bool silence_detection) { this->silence_detection_ = silence_detection; }
  
  // Trigger methods
  Trigger<> *get_listening_trigger() const { return this->listening_trigger_; }
  Trigger<> *get_end_trigger() const { return this->end_trigger_; }
  Trigger<> *get_start_trigger() const { return this->start_trigger_; }
  Trigger<> *get_wake_word_detected_trigger() const { return this->wake_word_detected_trigger_; }
  Trigger<std::string> *get_stt_end_trigger() const { return this->stt_end_trigger_; }
  Trigger<std::string> *get_tts_end_trigger() const { return this->tts_end_trigger_; }
  Trigger<std::string> *get_tts_start_trigger() const { return this->tts_start_trigger_; }
  Trigger<std::string, std::string> *get_error_trigger() const { return this->error_trigger_; }
  Trigger<> *get_idle_trigger() const { return this->idle_trigger_; }

  // Audio feedback
  void play_start_sound();
  void play_stop_sound();
  void play_error_sound();
  
 protected:
  // Core functionality
  bool allocate_buffers_();
  void clear_buffers_();
  void deallocate_buffers_();
  int read_microphone_();
  void set_state_(State state);
  void set_state_(State state, State desired_state);
  
  // State management
  void check_timeout_();
  void reset_state_();
  
  // Audio playback
#ifdef USE_SPEAKER
  void play_audio_file_(const char* file_data, size_t file_size);
#endif

  // MQTT client
  mqtt::MQTTClientComponent *mqtt_client_{nullptr};
  std::string audio_output_topic_{"voice/audio"};
  std::string state_topic_{"voice/state"};
  std::string command_topic_{"voice/command"};
  std::string event_topic_{"voice/event"};
  
  // Triggers
  Trigger<> *listening_trigger_ = new Trigger<>();
  Trigger<> *end_trigger_ = new Trigger<>();
  Trigger<> *start_trigger_ = new Trigger<>();
  Trigger<> *wake_word_detected_trigger_ = new Trigger<>();
  Trigger<std::string> *stt_end_trigger_ = new Trigger<std::string>();
  Trigger<std::string> *tts_end_trigger_ = new Trigger<std::string>();
  Trigger<std::string> *tts_start_trigger_ = new Trigger<std::string>();
  Trigger<std::string, std::string> *error_trigger_ = new Trigger<std::string, std::string>();
  Trigger<> *idle_trigger_ = new Trigger<>();

  // Hardware components
  microphone::Microphone *mic_{nullptr};
#ifdef USE_SPEAKER
  speaker::Speaker *speaker_{nullptr};
  uint8_t *speaker_buffer_{nullptr};
  size_t speaker_buffer_size_{0};
  size_t speaker_buffer_index_{0};
#endif
#ifdef USE_MEDIA_PLAYER
  media_player::MediaPlayer *media_player_{nullptr};
#endif

  // State flags
  bool local_output_{false};
  std::string conversation_id_{""};
  HighFrequencyLoopRequester high_freq_;

  // Audio processing 
#ifdef USE_ESP_ADF
  vad_handle_t vad_instance_;
  uint8_t vad_threshold_{5};
  uint8_t vad_counter_{0};
#endif
  std::unique_ptr<RingBuffer> ring_buffer_;
  float audio_gain_{1.0f};
  uint8_t *send_buffer_{nullptr};
  int16_t *input_buffer_{nullptr};

  // State control
  bool continuous_{false};
  bool silence_detection_{true};
  State state_{State::IDLE};
  State desired_state_{State::IDLE};
  uint32_t state_timestamp_{0};
  uint32_t timeout_duration_{60000}; // 60 seconds default timeout

  // Audio mode (HTTP or MQTT)
  AudioMode audio_mode_{AudioMode::AUDIO_MODE_HTTP};

#ifdef USE_MICRO_WAKE_WORD
  micro_wake_word::MicroWakeWord *micro_wake_word_{nullptr};
#endif
};

// Action class for starting voice assistant via automation
template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<VoiceAssistant> {
 public:
  void play(Ts... x) override {
    this->parent_->request_start(false, this->silence_detection_);
  }
  void set_silence_detection(bool silence_detection) { this->silence_detection_ = silence_detection; }
 protected:
  bool silence_detection_{true};
};

// Action class for continuous mode
template<typename... Ts> class StartContinuousAction : public Action<Ts...>, public Parented<VoiceAssistant> {
 public:
  void play(Ts... x) override { this->parent_->request_start(true, true); }
};

// Action class for stopping
template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<VoiceAssistant> {
 public:
  void play(Ts... x) override { this->parent_->request_stop(); }
};

// Condition class for checking if running
template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<VoiceAssistant> {
 public:
  bool check(Ts... x) override { return this->parent_->is_running(); }
};

// Global instance
extern VoiceAssistant *global_voice_assistant;

}  // namespace voice_assistant
}  // namespace esphome