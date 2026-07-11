#pragma once

#include <Arduino.h>

struct PropagationData {
  bool hasData;
  String sfi;
  String aIndex;
  String kIndex;
  String sunspots;
  String xray;
  String geomag;
  String signalNoise;
  String aurora;
  String fof2;
  String mufFactor;
  String band8040Day;
  String band8040Night;
  String band3020Day;
  String band3020Night;
  String band1715Day;
  String band1715Night;
  String band1210Day;
  String band1210Night;
  String band80m;
  String band40m;
  String band30m;
  String band20m;
  String band17m;
  String band15m;
  String band12m;
  String band10m;
  String updatedUtc;
  String status;
};

void propagationBegin();
bool refreshPropagationIfNeeded(bool wifiConnected);
void requestPropagationRefresh();
const PropagationData& getPropagationData();
