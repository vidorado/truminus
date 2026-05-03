#include "settings.hpp"
#include <type_traits>
#ifdef WEBSERVER
#include <ArduinoJson.h>
#endif
#ifdef CYD
#include <Preferences.h>
#endif

std::vector<String> BoilerMode = {"off","eco","high","boost"};
double BoilerTemp[] = {0.0, 40.0, 60.0, 60.0};
std::vector<String> FanModes = {"eco","high","off","1","2","3","4","5","6","7","8","9","10"};

void TMqttSetting::PublishValue(bool local)
{
    String value;
    switch (fkind) {
        case SKFloat:
           value=String(ffloatvalue,1);
           break;
        case SKInt:
           value=String(fintvalue);
           break;
        case SKString:
           value=fstringvalue;
           break;   
    }
    #ifndef NO_MQTT
    if (local) {
      mqttClient.publish(BaseTopicSet+ftopic, value, 0, fretain);
    }
    #endif
    #ifdef WEBSERVER
    sendToWebsocket(value);
    #endif
}

String TMqttSetting::getValueString()
{
    switch (fkind) {
        case SKFloat:  return String(ffloatvalue, 1);
        case SKInt:    return String(fintvalue);
        case SKString: return fstringvalue;
    }
    return "";
}

#ifdef WEBSERVER
void TMqttSetting::sendToWebsocket(const String &value)
{
    // Manual JSON via stack buffer to avoid heap allocation in hot path
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"command\":\"setting\",\"id\":\"%s\",\"value\":\"%s\"}",
             ftopic.c_str(), value.c_str());
    wsQueueSend(buf);
}
#endif

bool TMqttSetting::Validate(int newvalue)
{
    Serial.print("Accepted new int value ");
    Serial.print(newvalue);
    Serial.print(" for ");
    Serial.println(ftopic);
    return true;
}

bool TMqttSetting::Validate(double newvalue)
{
    Serial.print("Accepted new float value ");
    Serial.print(newvalue);
    Serial.print(" for ");
    Serial.println(ftopic);
    return true;
}

bool TMqttSetting::Validate(String newvalue)
{
    Serial.print("Accepted new string value ");
    Serial.print(newvalue);
    Serial.print(" for ");
    Serial.println(ftopic);
    return true;
}

TMqttSetting::TMqttSetting(String topic, SettingKind kind)
{
    ftopic=topic;
    fkind=kind;
    setADTopic(topic);
}

bool TMqttSetting::MqttMessage(String topic, String payload, boolean local)
{
    if (topic==BaseTopicSet+ftopic) {
        String value;
        switch (fkind) {
            case SKFloat:
              setValue(atof(payload.c_str()),local);
              break;
            case SKInt:
              setValue(atoi(payload.c_str()),local);
              break;
            case SKString:
              setValue(payload,local);
              break;    
        }
        return true;
    }
    return false;
}

void TMqttSetting::setValue(int newvalue, bool local)
{
    if (!Validate(newvalue)||fintvalue==newvalue) {
        return;
    }
    fintvalue=newvalue;
    PublishValue(local);
}

void TMqttSetting::setValue(double newvalue, bool local)
{
    if (!Validate(newvalue)||ffloatvalue==newvalue) {
        return;
    }
    ffloatvalue=newvalue;
#ifdef CYD
    if (fpersist) {
        Preferences prefs;
        prefs.begin("truminus", false);
        String key = ftopic.startsWith("/") ? ftopic.substring(1) : ftopic;
        prefs.putDouble(key.c_str(), ffloatvalue);
        prefs.end();
    }
#endif
    PublishValue(local);
}

void TMqttSetting::loadPersistedValue() {
#ifdef CYD
    if (!fpersist) return;
    Preferences prefs;
    prefs.begin("truminus", true);  // read-only
    String key = ftopic.startsWith("/") ? ftopic.substring(1) : ftopic;
    if (fkind == SKFloat) {
        double v = prefs.getDouble(key.c_str(), -9999.0);
        prefs.end();
        if (v > -9000.0) {
            ffloatvalue = v;  // bypass setValue — MQTT/WS not ready at setup
        }
        Serial.printf("NVS load %s = %.1f\n", ftopic.c_str(), ffloatvalue);
    } else if (fkind == SKString) {
        String v = prefs.getString(key.c_str(), "");
        prefs.end();
        if (v.length() > 0) {
            setValue(v, false);  // local=false: no MQTT echo; WS not open yet
        }
        Serial.printf("NVS load %s = %s\n", ftopic.c_str(), fstringvalue.c_str());
    } else {
        prefs.end();
    }
#endif
}

void TMqttSetting::setValue(String newvalue, bool local)
{
    if (!Validate(newvalue)||fstringvalue==newvalue) {
        return;
    }
    fstringvalue=newvalue;
#ifdef CYD
    if (fpersist) {
        Preferences prefs;
        prefs.begin("truminus", false);
        String key = ftopic.startsWith("/") ? ftopic.substring(1) : ftopic;
        prefs.putString(key.c_str(), fstringvalue);
        prefs.end();
    }
#endif
    PublishValue(local);
}

TBoilerSetting::TBoilerSetting(String topic) : TMqttSetting(topic, SKString) {
    setValue("off");
    setADComponent(CKSelect);
    setADName("Water mode");
    setADIcon("mdi:water-boiler");
    setADOptions(&BoilerMode);
};

bool TBoilerSetting::Validate(String newvalue)
{
    for (int i=0; i<BoilerMode.size(); i++ ) {
        if (newvalue==BoilerMode[i]) {
            setValue(BoilerTemp[i],false);
            return TMqttSetting::Validate(newvalue);
        }
    }
    Serial.print(newvalue);
    Serial.print(" is not a valid value for ");
    Serial.println(ftopic);
    return false;
}

TFanSetting::TFanSetting(String topic) : TMqttSetting(topic, SKString){
    setValue("off");
    setADComponent(CKSelect);
    setADName("Fan mode");
    setADIcon("mdi:fan");
    setADOptions(&FanModes);
};

bool TFanSetting::Validate(String newvalue)
{
    int num=atoi(newvalue.c_str());
    if (String(num)==newvalue && num>=0  && num<=10 ) {
        setValue(num,false);
        return TMqttSetting::Validate(newvalue);
    }
    if (newvalue=="off") {
        setValue(0,false);
        return TMqttSetting::Validate(newvalue);
    }
    if (newvalue=="eco") {
        setValue(-1,false);
        return TMqttSetting::Validate(newvalue);
    }
    if (newvalue=="high") {
        setValue(-2,false);
        return TMqttSetting::Validate(newvalue);
    }
    Serial.print(newvalue);
    Serial.print(" is not a valid value for ");
    Serial.println(ftopic);
    return false;
}

TOnOffSetting::TOnOffSetting(String topic) : TMqttSetting(topic, SKString){
    setValue(0);
    setADComponent(CKSwitch);
};

bool TOnOffSetting::Validate(String newvalue)
{
    int num=atoi(newvalue.c_str());
    if (String(num)==newvalue && num>=0  && num<=1 ) {
        setValue(num,false);
        return TMqttSetting::Validate(num);
    }
    Serial.print(newvalue);
    Serial.print(" is not a valid value for ");
    Serial.println(ftopic);
    return false;
}

TTempSetting::TTempSetting(String topic, double minvalue, double maxvalue) : TMqttSetting(topic, SKFloat) {
    fminvalue=minvalue;
    fmaxvalue=maxvalue;
    setValue(fminvalue);
    setADComponent(CKNumber);
    setADIcon("mdi:thermometer");
    setADUnit("°C");
    setADMin(minvalue);
    setADMax(maxvalue);
    setADMode("box");
    setADSuggested_display_precision(1);
};

bool TTempSetting::Validate(double newvalue)
{
    if (newvalue>=fminvalue && newvalue<=fmaxvalue) {
        return TMqttSetting::Validate(newvalue);
    }
    Serial.print("temperature ");
    Serial.print(newvalue);
    Serial.print(" out of range ");
    Serial.print(fminvalue);
    Serial.print(" - ");
    Serial.print(fmaxvalue);
    Serial.print(" for ");
    Serial.print(ftopic);
    return false;
}
