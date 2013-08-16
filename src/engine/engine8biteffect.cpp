#include "engine8biteffect.h"

#include "sampleutil.h"
#include "controlpushbutton.h"
#include "controlpotmeter.h"

Engine8bitEffect::Engine8bitEffect(const char* group) {
    potmeterDepth = new ControlPotmeter(ConfigKey(group, "8bitDepth"), 0., 1.);
    potmeterDepth->set(0.0f);
    potmeterDepth->setDefaultValue(0.0f);

    btnEnable = new ControlPushButton(ConfigKey(group, "8bit"));
    btnEnable->setButtonMode(ControlPushButton::TOGGLE);
}

Engine8bitEffect::~Engine8bitEffect() {
    delete potmeterDepth;
    delete btnEnable;
}

void Engine8bitEffect::process(const CSAMPLE *pIn, const CSAMPLE *pOut,
                               const int iBufferSize) {
    CSAMPLE* pOutput = (CSAMPLE*)pOut;

    float depth = potmeterDepth->get();
    int avg_samples = floor(pow(2.0, depth * 7.0f));

    if (btnEnable->get() == 0.0f || avg_samples == 0) {
        SampleUtil::copyWithGain(pOutput, pIn, 1.0f, iBufferSize);
        return;
    }

    float sum1 = 0.0f, sum2 = 0.0f, avg1 = 0.0f, avg2 = 0.0f;
    int j = 0;

    for (int i=0; i<iBufferSize; i+=2) {
        sum1 += pIn[i];
        sum2 += pIn[i+1];
        j++;

        if (j == avg_samples || i == iBufferSize-2) {
            avg1 = sum1 / avg_samples;
            avg2 = sum2 / avg_samples;
            sum1 = sum2 = 0.0f;
            j = 0;

            for (int k=i-avg_samples*2+2; k<=i; k+=2) {
                pOutput[k] = avg1;
                pOutput[k+1] = avg2;
            }
        }
    }
}
