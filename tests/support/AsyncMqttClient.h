#pragma once
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>
#include <set>

enum class AsyncMqttClientDisconnectReason : uint8_t { TCP_DISCONNECTED=0, MQTT_NOT_AUTHORIZED=5 };
struct AsyncMqttClientMessageProperties { uint8_t qos; bool dup, retain; };

class AsyncMqttClient {
 public:
  struct Publication { std::string topic, payload; uint8_t qos; bool retain; uint16_t id; };
  struct Subscription { std::string topic; uint8_t qos; uint16_t id; };
  static AsyncMqttClient *instance;
  std::vector<Publication> publications;
  std::vector<Subscription> subscriptions;
  std::set<uint16_t> pending;
  std::string host, user, password, id, willTopic, willPayload;
  uint16_t port=0;
  uint8_t willQos=0;
  bool willRetain=false, cleanSession=false, online=false, connecting=false, failPublish=false;
  unsigned connections=0;
  AsyncMqttClient() { instance=this; }
  AsyncMqttClient &setClientId(const char *value) { id=value; return *this; }
  AsyncMqttClient &setServer(const char *value,uint16_t number) { host=value;port=number;return *this; }
  AsyncMqttClient &setCleanSession(bool value) { cleanSession=value;return *this; }
  AsyncMqttClient &setKeepAlive(uint16_t) { return *this; }
  AsyncMqttClient &setMaxTopicLength(uint16_t) { return *this; }
  AsyncMqttClient &setCredentials(const char *name,const char *pass) { user=name?name:"";password=pass?pass:"";return *this; }
  AsyncMqttClient &setWill(const char *topic,uint8_t qos,bool retain,const char *payload) {
    willTopic=topic;willPayload=payload;willRetain=retain;willQos=qos;return *this;
  }
  void onConnect(std::function<void(bool)> fn) { connectCallback=fn; }
  void onDisconnect(std::function<void(AsyncMqttClientDisconnectReason)> fn) { disconnectCallback=fn; }
  void onSubscribe(std::function<void(uint16_t,uint8_t)> fn) { subscribeCallback=fn; }
  void onPublish(std::function<void(uint16_t)> fn) { publishCallback=fn; }
  void onMessage(std::function<void(char*,char*,AsyncMqttClientMessageProperties,size_t,size_t,size_t)> fn) { messageCallback=fn; }
  bool connected() const { return online; }
  void connect() { ++connections;connecting=true; }
  void establish() { online=true;connecting=false;connectCallback(false); }
  void disconnect(bool) { if(online||connecting)drop(); }
  void drop(uint8_t reason=0) {
    online=connecting=false;pending.clear();disconnectCallback(static_cast<AsyncMqttClientDisconnectReason>(reason));
  }
  bool clearQueue() { pending.clear();return !online; }
  uint16_t subscribe(const char *topic,uint8_t qos) {
    subscriptions.push_back({topic,qos,++packetId});return packetId;
  }
  void suback(uint16_t id,uint8_t qos=0) { subscribeCallback(id,qos); }
  uint16_t publish(const char *topic,uint8_t qos,bool retain,const char *payload) {
    if(failPublish||!online)return 0;
    publications.push_back({topic,payload,qos,retain,++packetId});pending.insert(packetId);return packetId;
  }
  void ack(uint16_t id) { pending.erase(id);publishCallback(id); }
  void message(std::string topic,std::string payload,bool retained=false,bool duplicate=false) {
    messageCallback(&topic[0],payload.empty()?nullptr:&payload[0],{0,duplicate,retained},payload.size(),0,payload.size());
  }
 private:
  uint16_t packetId=0;
  std::function<void(bool)> connectCallback;
  std::function<void(AsyncMqttClientDisconnectReason)> disconnectCallback;
  std::function<void(uint16_t,uint8_t)> subscribeCallback;
  std::function<void(uint16_t)> publishCallback;
  std::function<void(char*,char*,AsyncMqttClientMessageProperties,size_t,size_t,size_t)> messageCallback;
};
