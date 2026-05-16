#include "trumaframes.hpp"
#include "logs.hpp"
#include "globals.hpp"
#include <string.h>
#ifdef WEBSERVER
#include <ArduinoJson.h>
#include "webserver.hpp"
#endif
double RawKelvinToTemp(const uint16_t RawValue) {
  return RawValue / 10.0 - 273.0;
}

void TempToRawKelvin(double temp, uint8_t *dest)
{
    uint16_t rawvalue=htole16(uint16_t((temp+273.0)*10));
    memcpy(dest, &rawvalue, 2);
}
double RawToVoltage(const uint16_t RawValue)
{
    return RawValue/ 100.0 -327.67;
}

double RawToFlameTemperature(const uint8_t RawValue)
{
    double v=RawValue; 
    return pow(v,3.0)* 1.8602209820528515E-05 + pow(v, 2.0) * -0.0004895309102721512 + v * 1.4470709562301636 -65.64685821533203;
};

TMqttPublisherBase::TMqttPublisherBase(String topic)
{
    ftopic=topic;
    setADTopic(topic);
}

void TMqttPublisherBase::setValue(uint32_t newvalue)
{
    unsigned long now = millis();
    unsigned long elapsed = now-flastsent;
    boolean valuechanged = newvalue!=fvalue;
    fvalue=newvalue;
    String payload=getPayload();
    #ifdef WEBSERVER
    /* WebSocket: send when the value changes, when a new client just
       connected (fforcesend), OR at least every 10 s. The periodic
       refresh covers reload races where a client missed the connect
       burst and sits on stale data. AsyncWebSocket can handle the
       resulting ~2 msg/s without queue pressure. */
    if (valuechanged || fforcesend || elapsed > 10000) {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "{\"command\":\"status\",\"id\":\"%s\",\"value\":\"%s\"}",
                 ftopic.c_str(), payload.c_str());
        wsQueueSend(buf);
    }
    #endif
    if (valuechanged || fforcesend || elapsed>10000) {
        flastsent=now;
        //LOG_LIN_P(ftopic);
        //LOG_LIN_P(" ");
        //LOG_LIN_PL(fvalue);
        #ifndef NO_MQTT
        mqttClient.publish(BaseTopicStatus+ftopic,payload);
        #endif
    }
    fforcesend=false;
}
void TMqttPublisherBase::setForcesend(){
    fforcesend=true;
};

void TMqttPublisherBase::appendKeyValueJson(String& out) {
    out += "\"";
    out += ftopic;
    out += "\":\"";
    out += getPayload();
    out += "\"";
}

TFrame14::TFrame14()  : TFrameBase()
{
    fframeid=0x14;
    fpublishers.push_back(new TPubTemperature("/f14_roomtemp"));
    fpublishers.push_back(new TPubTemperature("/f14_roomtargettemp"));
    fpublishers.push_back(new TPubTemperature("/f14_watertargettemp"));
}

void TFrame14::publishFrameData()
{
   frame14Data *locdata=(frame14Data *)&fdata;
   fpublishers[0]->setValue(le16toh(locdata->RoomTemperature));
   fpublishers[1]->setValue(le16toh(locdata->RoomTargetTemperature));
   fpublishers[2]->setValue(le16toh(locdata->WaterTargetTemperature));
}


TFrameBase::TFrameBase()
{
    fdataok = false;
}

void TFrameBase::setReadResult(boolean ok)
{
    if (fdataok!=ok) {
        fdataok=ok;
        LOG_LIN_P("read frame ");
        LOG_LIN_P(fframeid,16);
        if (ok) {
            LOG_LIN_PL(" ok");
        } else {
            LOG_LIN_PL(" error");
        }
    }
}

void TFrameBase::setData(uint8_t *data)
{
    memcpy(fdata, data,8);
    publishFrameData();
}

void TFrameBase::setForcesend()
{
    for (int i=0; i<fpublishers.size(); i++) {
        fpublishers[i]->setForcesend();
    }
}

void TFrameBase::appendKeyValueJson(String& out)
{
    for (int i=0; i<fpublishers.size(); i++) {
        if (out.length() > 0 && out[out.length()-1] != '{') out += ",";
        fpublishers[i]->appendKeyValueJson(out);
    }
}

void TFrameBase::getData(uint8_t *dest)
{
    memcpy(dest, &fdata, 8);
}

void TFrameBase::publishFrameData()
{
    //do nothing in base class
}

void TFrameBase::PublishAutoDiscovery()
{
    for (int i=0; i<fpublishers.size(); i++) {
        fpublishers[i]->PublishAutoDiscovery();
    }
}

TFrame16::TFrame16():TFrameBase()
{
    fframeid=0x16;
    FWaterDemand=false;
    FWaterTemp=-273.0;
    FRoomTemp=-273.0;
    fpublishers.push_back(new TPubBool("/antifreeze"));
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("Antifreeze")
      ->setADIcon("mdi:snowflake")
      ->setADDevice_class("safety")
      ->setADPayload_off("1")
      ->setADPayload_on("0");
    fpublishers.push_back(new TPubBool("/supply220"));
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("220V supply")
      ->setADIcon("mdi:power-plug")
      ->setADDevice_class("power");
    fpublishers.push_back(new TPubBool("/window"));
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("Window")
      ->setADIcon("mdi:window-closed-variant")
      ->setADDevice_class("window")
      ->setADPayload_off("1")
      ->setADPayload_on("0");
    fpublishers.push_back(new TPubBool("/roomdemand"));
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("Heating active")
      ->setADIcon("mdi:fire")
      ->setADDevice_class("heat");
    fpublishers.push_back(new TPubBool("/waterdemand"));
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("Boiler active")
      ->setADIcon("mdi:water-boiler-auto")
      ->setADDevice_class("heat");
    fpublishers.push_back(new TPubBool("/error")); 
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("System error")
      ->setADIcon("mdi:alert")
      ->setADDevice_class("problem");
    fpublishers.push_back(new TPubTemperature("/room_temp"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Room temperature")
      ->setADIcon("mdi:home-thermometer")
      ->setADDevice_class("temperature")
      ->setADUnit("°C");
    fpublishers.push_back(new TPubTemperature("/water_temp"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Water temperature")
      ->setADIcon("mdi:water-thermometer")
      ->setADDevice_class("temperature")
      ->setADUnit("°C");
    fpublishers.push_back(new TPubVoltage("/voltage"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Supply voltage")
      ->setADIcon("mdi:car-battery")
      ->setADDevice_class("voltage")
      ->setADUnit("V")
      ->setADSuggested_display_precision(1)
      ->setADValue_template("{{ value | float(0) | round(1) }}");
}

void TFrame16::publishFrameData()
{
    frame16Data *locdata=(frame16Data *) &fdata;
    fpublishers[0]->setValue(locdata->Antifreeze);
    fpublishers[1]->setValue(locdata->Supply220);
    fpublishers[2]->setValue(locdata->Window);
    fpublishers[3]->setValue(locdata->RoomDemand);
    fpublishers[4]->setValue(locdata->WaterDemand);
    fpublishers[5]->setValue(locdata->Error);
    fpublishers[6]->setValue(le16toh(locdata->RoomTemperature));
    fpublishers[7]->setValue(le16toh(locdata->WaterTemperature));
    fpublishers[8]->setValue(le16toh(locdata->BatteryVoltage));
    FWaterDemand=locdata->WaterDemand;
    FWaterTemp=RawKelvinToTemp(le16toh(locdata->WaterTemperature));
    FRoomTemp=RawKelvinToTemp(le16toh(locdata->RoomTemperature));
}

void TFrame21::publishFrameData()
{
    // Confirmed byte layout (verified by heating room sensor and boiler tests):
    //   byte 0   = Kelvin×10 bits 7:0 (LSB of 12-bit value)
    //   byte 1   = bits 3:0 → Kelvin×10 bits 11:8 (MSB nibble)
    //              bits 7:4 → 4-bit rolling message counter (0→F→0, ~8-9 s/step in normal op)
    //              NOT flags — the counter causes bit4 to alternate every ~9 s (false waterdemand noise)
    //   byte 2 = water temperature (12-bit Kelvin×10, bits 11:4 of the 12-bit value)
    //   byte 7 = unknown
    uint8_t flags = fdata[1];
    fpublishers[0]->setValue((flags >> 0) & 1);
    fpublishers[1]->setValue((flags >> 1) & 1);
    fpublishers[2]->setValue((flags >> 2) & 1);
    fpublishers[3]->setValue((flags >> 3) & 1);
    // fpublishers[4] (/waterdemand): bit4 is counter LSB, not a real flag — skip to avoid noise
    fpublishers[5]->setValue((flags >> 7) & 1);
    FWaterDemand = false;  // real water demand is in frame 0x22 (TFrame22)

    // 12-bit Kelvin×10: byte0 = bits 7:0 (LSB), byte1 bits 3:0 = bits 11:8 (MSB)
    uint16_t rawTemp = (uint16_t)fdata[0] | ((uint16_t)(fdata[1] & 0x0F) << 8);
    FRoomTemp  = RawKelvinToTemp(rawTemp);
    if (FRoomTemp  < 0.0 || FRoomTemp  > 50.0) FRoomTemp  = -273.0;

    // Water temperature: 12-bit Kelvin×10 at offset 12 bits
    // nibble high of byte1 (bits 7:4) + byte2
    uint16_t rawWater = (uint16_t)(fdata[1] >> 4) | ((uint16_t)fdata[2] << 4);
    FWaterTemp = RawKelvinToTemp(rawWater);
    if (FWaterTemp < 0.0 || FWaterTemp > 100.0) FWaterTemp = -273.0;

    fpublishers[6]->setValue((uint16_t)((FRoomTemp  + 273.0) * 10.0));
    if (FWaterTemp > -273.0)
        fpublishers[7]->setValue((uint16_t)((FWaterTemp + 273.0) * 10.0));
    else
        fpublishers[7]->setValue(0);
    fpublishers[8]->setValue(0);  // voltage not available in frame 0x21
}

TFrame22::TFrame22() : TFrameBase() {
    fframeid = 0x22;
    fpublishers.push_back(new TPubBool("/water_heating"));
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("Water heating active")
      ->setADIcon("mdi:water-boiler");
}

void TFrame22::publishFrameData() {
    FWaterHeating = (fdata[1] & 0xC0) == 0x40;  // bit6=1, bit7=0 → burner active (0x00=off, 0x40/0x50=heating, 0xD0=idle)
    fpublishers[0]->setValue(FWaterHeating ? 1u : 0u);
}

TFrameSetTemp::TFrameSetTemp(uint8_t frameid)
{
    fframeid = frameid;
    setTemperature(0.0);
}

void TFrameSetTemp::setTemperature(double temp)
{
    ftemp = temp;
    //LOG_LIN_P("Setting temperature to ");
    //LOG_LIN_PL(ftemp);
    TempToRawKelvin(ftemp,&fdata[0]);
}

TFrame34::TFrame34() : TFrameBase()
{
    fframeid=0x34;
    //operation time both raw and decoded
    fpublishers.push_back(new TPubOperationTime("/operation_time"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Operation time")
      ->setADIcon("mdi:clock-check")
      ->setADEntity_category("diagnostic")
      ->setADValue_template("{% set total_min = value | int %}{% set days = (total_min // 1440) %}{% set hours = (total_min % 1440) // 60 %}{% set minutes = total_min % 60 %}{{ days }}d {{ hours }}h {{ minutes }}m");
    fpublishers.back()->addAutoDiscovery("_raw")->setADComponent(CKSensor)
      ->setADName("Operation time (raw)")
      ->setADIcon("mdi:clock-outline")
      ->setADDevice_class("duration")
      ->setADUnit("min")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubBool("/k1"));
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("Relay K1")
      ->setADIcon("mdi:dip-switch")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubBool("/k2"));
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("Relay K2")
      ->setADIcon("mdi:dip-switch")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubBool("/k3"));
    fpublishers.back()->setADComponent(CKBinary_sensor)
      ->setADName("Relay K3")
      ->setADIcon("mdi:dip-switch")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubEbtMode("/ebtmode"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("EBT Mode")
      ->setADIcon("mdi:cog")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubHydronicStartInfo("/hydr_start_info"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Hydronic start info")
      ->setADIcon("mdi:information")
      ->setADEntity_category("diagnostic");
}

void TFrame34::publishFrameData()
{
    frame34Data *locdata=(frame34Data *) &fdata;
    fpublishers[0]->setValue(locdata->OperationTime[0]*65535+locdata->OperationTime[1]*256+locdata->OperationTime[2]);
    fpublishers[1]->setValue(locdata->K1);
    fpublishers[2]->setValue(locdata->K2);
    fpublishers[3]->setValue(locdata->K3);
    fpublishers[4]->setValue(locdata->EbtMode);
    fpublishers[5]->setValue(locdata->Event2);
}

TFrame37::TFrame37(): TFrameBase()
{
    fframeid = 0x37;
    fpublishers.push_back(new TPubFlameTemperature("/trend_value_hydronic"));
}

void TFrame37::publishFrameData()
{
    frame37Data *locdata=(frame37Data *) &fdata;
    fpublishers[0]->setValue(locdata->TrendValueHydronic);
}

TFrame39::TFrame39(): TFrameBase()
{
    fframeid=0x39;
    fpublishers.push_back(new TPubBlowOutTemperature("/blow_out_temp"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Blow out temperature")
      ->setADIcon("mdi:thermometer")
      ->setADDevice_class("temperature")
      ->setADUnit("°C")
      ->setADSuggested_display_precision(1)
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubPumpFrequency("/pump_freq"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Pump frequency")
      ->setADIcon("mdi:sine-wave")
      ->setADUnit("Hz")
      ->setADSuggested_display_precision(2)
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubFlameTemperature("/flame_temp"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Flame temperature")
      ->setADIcon("mdi:fire-circle")
      ->setADDevice_class("temperature")
      ->setADUnit("°C")
      ->setADSuggested_display_precision(1)
      ->setADEntity_category("diagnostic");
}

void TFrame39::publishFrameData()
{
    frame39Data *locdata=(frame39Data *) &fdata;
    fpublishers[0]->setValue(le16toh(locdata->BlowOutTemperature));
    fpublishers[1]->setValue(locdata->PumpFrequency);
    fpublishers[2]->setValue(locdata->FlameTemperature);
}

TFrame35::TFrame35(): TFrameBase()
{
    fframeid = 0x35;
    fpublishers.push_back(new TPubBurnerFanVoltage("/burner_fan_v"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Burner fan voltage")
      ->setADIcon("mdi:fan-chevron-up")
      ->setADDevice_class("voltage")
      ->setADUnit("V")
      ->setADSuggested_display_precision(1)
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubBurnerStatus("/burner_status"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Burner status")
      ->setADIcon("mdi:fire")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubGlowPlugStatus("/glow_plug_status"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Glow plug status")
      ->setADIcon("mdi:radiator")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubHydronicState("/hydronic_state"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Hydronic state")
      ->setADIcon("mdi:state-machine")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubHydronicFlame("/hydronic_flame"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Hydronic flame")
      ->setADIcon("mdi:fire-alert")
      ->setADEntity_category("diagnostic");
}

void TFrame35::publishFrameData()
{
    frame35Data *locdata=(frame35Data *) &fdata;
    fpublishers[0]->setValue(locdata->BurnerFanVoltage);
    fpublishers[1]->setValue(locdata->AV3_Hydronic);
    fpublishers[2]->setValue(locdata->AV2_Hydronic);
    fpublishers[3]->setValue(locdata->EVENT0_Hydronic);
    fpublishers[4]->setValue(locdata->EVENT0_Hydronic);
}

TFrame3b::TFrame3b(): TFrameBase()
{
    fframeid = 0x3b;
    fpublishers.push_back(new TPubBattery("/battery"));
    fpublishers.push_back(new TPubExtractorFanRpm("/extractor_fan_rpm"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Extractor fan RPM")
      ->setADIcon("mdi:fan-speed-3")
      ->setADUnit("RPM")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TMqttPublisherBase("/current_error_hydronic"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Current error hydronic")
      ->setADIcon("mdi:alert-octagon")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubPumpSafetySwitch("/pump_safety_switch"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Pump safety switch")
      ->setADIcon("mdi:toggle-switch")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TMqttPublisherBase("/circ_air_motor_setpoint"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Air circulation setpoint")
      ->setADIcon("mdi:fan")
      ->setADUnit("%")
      ->setADEntity_category("diagnostic");
    fpublishers.push_back(new TPubCircAirMotorCurrent("/circ_air_motor_current"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Air circulation current")
      ->setADIcon("mdi:current-ac")
      ->setADDevice_class("current")
      ->setADUnit("A")
      ->setADSuggested_display_precision(1)
      ->setADEntity_category("diagnostic");

}

void TFrame3b::publishFrameData()
{
    frame3bData *locdata=(frame3bData *) &fdata;
    fpublishers[0]->setValue(locdata->Battery);
    fpublishers[1]->setValue(locdata->ExtractorFanRpm);
    fpublishers[2]->setValue(locdata->CurrentErrorHydronic);
    fpublishers[3]->setValue(locdata->CurrentErrorHydronic);
    fpublishers[4]->setValue(locdata->CircAirMotor_Setpoint);
    fpublishers[5]->setValue(locdata->CircAirMotorCurrent);

}

TFrameSetFan::TFrameSetFan(uint8_t frameid)
{
    fframeid=frameid;
    setPumpOrFan(0x10);
}

void TFrameSetFan::setPumpOrFan(byte PumpOrFan)
{
    fPumpOrFan=PumpOrFan;
    fdata[0]=PumpOrFan | 0xe0;
    fdata[1]=0xfe;
}

TFrameEnergySelect::TFrameEnergySelect(uint8_t frameid)
{
    fframeid=frameid;
    setEnergySelection(EsGasDiesel);
}

void TFrameEnergySelect::setEnergySelection(TEnergySelection EnergySelection)
{
    TEnergyPriority priorities[] = {EpFuel, EpBothPrioFuel, EpBothPrioFuel, EpBothPrioElectro, EpBothPrioElectro};
    fEnergySelection=EnergySelection;
    fdata[0]=priorities[fEnergySelection];
}

TFrameSetPowerLimit::TFrameSetPowerLimit(uint8_t frameid)
{
    fframeid=frameid;
    setPowerLimit(EsGasDiesel);
}

void TFrameSetPowerLimit::setPowerLimit(TEnergySelection EnergySelection)
{
    uint16_t limits[] = {0,900,1800, 900,1800};
    fEnergySelection=EnergySelection;
    uint16_t limit=htole16(limits[fEnergySelection]);
    memcpy(&fdata[0],&limit,2);
}

TFrameSetControlElements::TFrameSetControlElements(uint8_t frameid)
{
    fframeid=frameid;
    SetSummerWinterMode(SWSummer60);
    SetElectroGasMix(GMGas);
    SetTempSetpoint(0);
    SetDiensteLin(0);
}

void TFrameSetControlElements::SetSummerWinterMode(TSummerWinterMode SummerWinterMode)
{
    fSummerWinterMode=SummerWinterMode;
    fdata[0] = fdata[0] & 0xf0 | fSummerWinterMode;
}

void TFrameSetControlElements::SetElectroGasMix(TElectroGasMixMode ElectroGasMixMode)
{
    fElectroGasMixMode=ElectroGasMixMode;
    fdata[0] = fdata[0] & 0x0f | (fElectroGasMixMode << 4);
}

void TFrameSetControlElements::SetTempSetpoint(uint16_t TempSetpoint)
{
    fTempSetpoint=TempSetpoint;
    uint16_t temp=htole16(fTempSetpoint);
    memcpy(&fdata[1],&temp,2);
}

void TFrameSetControlElements::SetDiensteLin(uint8_t DiensteLin)
{
    fDiensteLin=DiensteLin;
    fdata[2]=fDiensteLin;
}

TMasterFrame::TMasterFrame(uint8_t nad, uint8_t len, uint8_t sid ):TFrameBase()
{
   
   fenabled=true; 
   fnad=nad;
   flen=len;
   fsid=sid;
   fdata[0]=fnad;
   fdata[1]=flen; //here it's always single frame
   fdata[2]=fsid;
   for (int i=3; i<8; i++) {
    fdata[i]=0xff;
   }   
}

void TMasterFrame::setData(uint8_t *data)
{
    memcpy(freply, data,8);
    setReadResult(CheckReply());
}

void TMasterFrame::getData(uint8_t *dest)
{
    memcpy(dest, &fdata, 8);
}

void TMasterFrame::setReadResult(boolean ok)
{
    if (fdataok!=ok) {
        fdataok=ok;
        LOG_LIN_P("read master frame ");
        LOG_LIN_P(fsid,16);
        if (ok) {
            LOG_LIN_PL(" ok");
        } else {
            LOG_LIN_PL(" error");
        }
    }
}

bool TMasterFrame::CheckReply()
{
    return freply[2]==fsid+64;
}

uint8_t getProtectedID(uint8_t FrameID)
{
    // calc Parity Bit 0
    uint8_t p0 = bitRead(FrameID, 0) ^ bitRead(FrameID, 1) ^ bitRead(FrameID, 2) ^ bitRead(FrameID, 4);
    // calc Parity Bit 1
    uint8_t p1 = ~(bitRead(FrameID, 1) ^ bitRead(FrameID, 3) ^ bitRead(FrameID, 4) ^ bitRead(FrameID, 5));
    // combine bits to protected ID
    // 0..5 id is limited between 0x00..0x3F
    // 6    parity bit 0
    // 7    parity bit 1
    return ((p1 << 7) | (p0 << 6) | (FrameID & 0x3F));
}

TAssignFrameRanges::TAssignFrameRanges(uint8_t startindex, std::array<uint8_t,4> frames)
  :TMasterFrame(0x01, 0x06, 0xb7) 
{
    fdata[3]=startindex;
    for (int i=0; i<4; i++) {
        fdata[i+4]=getProtectedID(frames[i]);
    }
}

bool TOnOff::CheckReply()
{
    if (!TMasterFrame::CheckReply()) {
        return false;
    }
    frequestedstate = freply[3];
    fcurrentstate = freply[4];
    /*
    LOG_LIN_P("req.state ");
    LOG_LIN_P(frequestedstate,16);
    LOG_LIN_P(" current ");
    LOG_LIN_PL(fcurrentstate, 16);
    */
    fpublishers[0]->setValue(frequestedstate);
    fpublishers[1]->setValue(fcurrentstate);
    return true;
}

TOnOff::TOnOff():TMasterFrame(0x01, 0x06, 0xb8)
{
    fdata[3]=0x20;
    fdata[4]=0x03;
    fdata[6]=0x00;
    fpublishers.push_back(new TMqttPublisherBase("/requested_state"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Requested state")
      ->setADIcon("mdi:sync-circle")
      ->setADEntity_category("diagnostic")
      ->setADValue_template("{% set mapper = {0:\"no tin\", 1:\"idle\", 2:\"on\", 3:\"shutdown\", 4:\"powering up\"} %}{{ mapper.get(value | int, \"unknown\") }} ({{ value }})");
    fpublishers.push_back(new TMqttPublisherBase("/current_state"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Current state")
      ->setADIcon("mdi:sync-circle")
      ->setADEntity_category("diagnostic")
      ->setADValue_template("{% set mapper = {0:\"no tin\", 1:\"idle\", 2:\"on\", 3:\"shutdown\", 4:\"powering up\"} %}{{ mapper.get(value | int, \"unknown\") }} ({{ value }})");
    SetOn(false);
}

void TOnOff::SetOn(bool on)
{
    fon=on;
    if (fon) {
        fdata[5]=0x01;
    } else {
        fdata[5]=0x00;
    }
}

TGetErrorInfo::TGetErrorInfo():TMasterFrame(0x7F, 0x06, 0xb2)
{
    ferrorClass=0;
    ferrorCode=0;
    ferrorShort=0;
    fdata[3]=0x23;
    fdata[4]=0x17;
    fdata[5]=0x46;
    fdata[6]=0x20;
    fdata[7]=0x03;
    fpublishers.push_back(new TMqttPublisherBase("/err_class"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Error class")
      ->setADIcon("mdi:alert-circle-outline")
      ->setADValue_template("{% set val = value | int %}{% if val == 0 %}no error{% elif val in [1, 2] %}warning{% elif val in [10, 20, 30] %}error{% elif val == 40 %}locked{% else %}unknown{% endif %} ({{ val }})");
    fpublishers.push_back(new TMqttPublisherBase("/err_code"));
    fpublishers.back()->setADComponent(CKSensor)
      ->setADName("Error code")
      ->setADIcon("mdi:numeric");
    fpublishers.push_back(new TMqttPublisherBase("/err_short"));
}

bool TGetErrorInfo::CheckReply()
{
    if (!TMasterFrame::CheckReply()) {
     return false;
    }
    ferrorClass=freply[4];
    ferrorCode=freply[5];
    ferrorShort=freply[6];
    /*
    LOG_LIN_P("err.type ");
    LOG_LIN_P(ferrorClass,16);
    LOG_LIN_P(" code ");
    LOG_LIN_P(ferrorCode,16);
    LOG_LIN_P(" short ");
    LOG_LIN_PL(ferrorShort,16);
    */
    fpublishers[0]->setValue(ferrorClass);
    fpublishers[1]->setValue(ferrorCode);
    fpublishers[2]->setValue(ferrorShort); 
    return true;
}
