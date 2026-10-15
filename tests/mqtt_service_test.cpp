#include "mqtt_service.h"
#include <assert.h>

uint32_t now=0;
uint32_t millis() { return now; }
AsyncMqttClient *AsyncMqttClient::instance=nullptr;
dns_found_callback dnsCallback=nullptr;
void *dnsArgument=nullptr;
unsigned dnsCalls=0;
err_t dns_gethostbyname(const char *,ip_addr_t *,dns_found_callback callback,void *argument) {
  ++dnsCalls;dnsCallback=callback;dnsArgument=argument;return ERR_INPROGRESS;
}
static void resolveDns(uint32_t address) {
  const ip_addr_t result={address};
  dnsCallback("test",&result,dnsArgument);
}

struct Fixture {
  DeskMonitor desk;
  PanelBridge panel;
  TargetMove target;
  DeskMqtt mqtt;
  MqttSettings settings;
  AsyncMqttClient &client;
  Fixture(const char *host="192.168.1.50") : mqtt(desk,panel,target), client(*AsyncMqttClient::instance) {
    now=0;
    settings.enabled=true;
    strcpy(settings.host,host);
    strcpy(settings.username,"desk");strcpy(settings.password,"test-password");
    strcpy(settings.prefix,"home/office/desk");
    mqtt.begin("standing-desk-abc123",settings);
  }
  void subscribe() {
    client.establish();mqtt.service(true);
    assert(client.subscriptions.size()>=2);
    for(unsigned i=client.subscriptions.size()-2;i<client.subscriptions.size();++i) {
      assert(client.subscriptions[i].qos==0);
      client.suback(client.subscriptions[i].id);
    }
    mqtt.service(true);
  }
  void drain() {
    unsigned count=0;
    while(!client.pending.empty()) {
      assert(client.pending.size()==1 && ++count<30);
      client.ack(*client.pending.begin());mqtt.service(true);
    }
  }
  void connect() { mqtt.service(true);subscribe();drain();assert(mqtt.connected()); }
};

int main() {
  {
    Fixture f;
    f.mqtt.service(false);assert(f.client.connections==0);
    f.mqtt.service(true,false);assert(f.client.connections==0);
    f.connect();
    assert(f.client.cleanSession && f.client.willRetain);
    assert(f.client.willTopic=="home/office/desk/availability" && f.client.willPayload=="offline");
    unsigned discovery=0;
    for(const auto &message:f.client.publications) {
      assert(message.retain && message.qos==1);
      if(message.topic.find("homeassistant/")==0)++discovery;
    }
    assert(discovery==10);
    f.client.message("homeassistant/status","online",true);
    f.mqtt.service(true);f.drain();
    discovery=0;
    for(const auto &message:f.client.publications)if(message.topic.find("homeassistant/")==0)++discovery;
    assert(discovery==20);
    f.client.message("home/office/desk/command/move","up");
    assert(f.panel.commandButton==0); // A network callback does not touch GPIO/desk control.
    f.mqtt.service(true);assert(f.panel.commandButton==0x20 && f.mqtt.accepted()==1);
    now=300;assert(!f.panel.renew(f.panel.commandId,0x20,now));
    assert(f.panel.commandButton==0);
  }
  {
    Fixture f;f.connect();
    f.client.message("home/office/desk/command/preset","1",true);
    f.mqtt.service(true);assert(f.panel.commandButton==0 && f.mqtt.rejected()==1);
    f.client.message("home/office/desk/command/preset","1",false,true);
    f.mqtt.service(true);assert(f.panel.commandButton==0 && f.mqtt.rejected()==2);
    f.client.message("home/office/desk/command/move","down");
    f.panel.inputOverflow(now);
    f.mqtt.service(true);assert(f.panel.commandButton==0 && f.mqtt.rejected()==3);
    f.client.message("home/office/desk/command/move","up");
    now=250;f.mqtt.service(true);assert(f.panel.commandButton==0 && f.mqtt.rejected()==4);
  }
  {
    Fixture f;f.connect();
    f.client.message("home/office/desk/command/preset","2");
    f.client.drop();f.mqtt.service(true);
    now=5000;f.mqtt.service(true);f.subscribe();f.drain();
    assert(f.panel.commandButton==0 && f.mqtt.accepted()==0); // No replay on reconnect.
    f.client.message("home/office/desk/command/move","up");
    f.mqtt.service(true);assert(f.panel.commandButton==0x20);
    f.mqtt.service(false);assert(f.panel.commandButton==0); // Broker-owned short hold released on loss.
  }
  {
    Fixture f;f.mqtt.service(true);f.subscribe();
    assert(f.client.pending.size()==1);
    for(now=1000;now<10000;now+=1000)f.mqtt.service(true);
    assert(f.client.publications.size()==1); // No growing state/discovery queue without PUBACKs.
    f.mqtt.service(true);
    assert(!strcmp(f.mqtt.error(),"publish_timeout"));
    assert(!f.mqtt.connected());
  }
  {
    Fixture f;f.mqtt.service(true);f.client.drop(5);f.mqtt.service(true);
    assert(!strcmp(f.mqtt.error(),"not_authorized"));
  }
  {
    Fixture f;
    const uint8_t height[]={0x5A,0x07,0xBF,0x3F,0x10,0x15};
    for(uint8_t byte:height)f.desk.feed(byte,now);
    struct IdlePort {
      unsigned position=0;
      int available() const { return 5-position; }
      int read() { return position++==0?0xA5:0; }
      unsigned write(uint8_t) { return 1; }
    } port;
    f.panel.forward(port,now);
    f.connect();
    f.client.message("home/office/desk/command/target","95");
    f.mqtt.service(true);
    assert(f.target.active() && f.panel.commandButton==0x20);
    f.client.message("home/office/desk/command/cancel","cancel");
    f.mqtt.service(true);
    assert(!f.target.active() && f.panel.commandButton==0);
    f.drain();
    now=1000;f.mqtt.service(true);f.drain();
    assert(f.client.publications.back().payload.find("\"height_cm\":70.00")!=std::string::npos);
    assert(f.client.publications.back().payload.find("\"height_fresh\":false")!=std::string::npos);
    f.client.message("home/office/desk/command/target","95");
    f.mqtt.service(true);
    assert(!f.target.active() && f.mqtt.rejected()==1);
  }
  {
    Fixture f;f.mqtt.service(true);now=10000;f.mqtt.service(true);
    assert(!strcmp(f.mqtt.error(),"connect_timeout"));
  }
  {
    Fixture f;f.connect();
    f.settings.enabled=false;
    f.mqtt.configure(f.settings);f.mqtt.service(true);
    now=100;f.mqtt.service(true);
    assert(!strcmp(f.mqtt.stateName(),"disabled") && !f.mqtt.connected());
    f.client.message("home/office/desk/command/move","up");
    f.mqtt.service(true);assert(f.panel.commandButton==0);
  }
  {
    dnsCalls=0;
    Fixture f("oldbox.lan");f.mqtt.service(true);
    assert(dnsCalls==1 && f.client.connections==0);
    strcpy(f.settings.host,"newbox.lan");
    f.mqtt.configure(f.settings);f.mqtt.service(true);
    now=100;f.mqtt.service(true);
    resolveDns(0x3201A8C0); // Old lookup must not connect with the new settings.
    f.mqtt.service(true);
    assert(dnsCalls==2 && f.client.connections==0);
    resolveDns(0x3301A8C0);
    f.mqtt.service(true);
    assert(f.client.connections==1 && f.client.host=="192.168.1.51");
  }
  puts("All MQTT connection and command integration tests passed.");
}
