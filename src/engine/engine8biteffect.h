#ifndef ENGINE8BITEFFECT_H
#define ENGINE8BITEFFECT_H

#include "engineobject.h"

class ControlObject;
class ControlPushButton;

class Engine8bitEffect : public EngineObject {
  public:
    Engine8bitEffect(const char* group);
    ~Engine8bitEffect();

    void process(const CSAMPLE* pIn, const CSAMPLE* pOut, const int iBufferSize);
  private:
    ControlObject* potmeterDepth;
    ControlPushButton* btnEnable;
};

#endif // ENGINE8BITEFFECT_H
