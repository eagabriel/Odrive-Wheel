// Compiled together with the production sources by test_forge_v108.py.
static void start(HidFFB& hid, uint8_t slot=1) {
    FFB_EffOp_Data_t op{};
    op.effectBlockIndex=slot;
    op.reportId=HID_ID_EFOPREP;
    op.state=1;
    op.loopCount=1;
    hid.hidOut(HID_ID_EFOPREP,HID_REPORT_TYPE_OUTPUT,reinterpret_cast<const uint8_t*>(&op),sizeof(op));
}
static uint8_t status(HidFFB& hid) {
    uint8_t value=0;
    assert(hid.hidGet(HID_ID_STATE,HID_REPORT_TYPE_INPUT,&value,1)==1);
    return value;
}
static void configure(HidFFB& hid, uint8_t type, uint16_t duration=0xffff, uint16_t delay=0) {
    FFB_SetEffect_t report{};
    report.effectBlockIndex=1;
    report.effectType=type;
    report.duration=duration;
    report.startDelay=delay;
    report.enableAxis=1;
    report.directionX=36000;
    hid.hidOut(HID_ID_EFFREP,HID_REPORT_TYPE_OUTPUT,reinterpret_cast<const uint8_t*>(&report),sizeof(report));
}
static void near(int32_t actual, int32_t expected) { assert(std::abs(actual-expected)<=1); }

int main() {
    auto calc=std::make_shared<EffectsCalculator>();
    HidFFB hid(calc,1);
    std::vector<std::unique_ptr<Axis>> axes;
    axes.emplace_back(new ODriveLocalAxis);
    auto& axis=static_cast<ODriveLocalAxis&>(*axes[0]);
    axis.endstopStrength_=0;
    axis.getMetrics()->pos_scaled_16b=1000;
    axis.getMetrics()->speed=20;
    axis.getMetrics()->accel=100;

    // HID gain scales every condition once, including filtered paths. Full
    // gain preserves existing forces; zero mutes, while local effects bypass it.
    for (uint8_t type : {FFB_EFFECT_SPRING,FFB_EFFECT_DAMPER,FFB_EFFECT_FRICTION,FFB_EFFECT_INERTIA}) {
        for (bool filtered : {false,true}) {
            FFB_Effect effect;
            effect.type=type;
            effect.axisMagnitudes[0]=1;
            effect.conditions[0].positiveCoefficient=32767;
            effect.conditions[0].negativeCoefficient=32767;
            effect.conditions[0].positiveSaturation=32767;
            effect.conditions[0].negativeSaturation=32767;
            if(filtered) calc->setFilters(&effect);
            int full=0;
            for(int i=0;i<1000;i++) full=calc->calcComponentForce(&effect,0,axes,0);
            assert(full<0);
            effect.gain=128;
            near(calc->calcComponentForce(&effect,0,axes,0),(int64_t)full*128/255);
            effect.gain=0;
            assert(calc->calcComponentForce(&effect,0,axes,0)==0);
            effect.gain=255;
            near(calc->calcComponentForce(&effect,0,axes,0),full);
            for(int flags=0;flags<4;flags++) {
                axis.axisInverted_=(flags&1)!=0;
                axis.ffbInverted_=(flags&2)!=0;
                axis.setEffectTorque(calc->calcComponentForce(&effect,0,axes,0));
                near(axis.getMetrics()->torque,(flags&2)?-full:full);
            }
        }
    }
    axis.axisInverted_=false;
    axis.ffbInverted_=false;
    // Noncondition gain is not accidentally applied twice.
    FFB_Effect constant;
    constant.type=FFB_EFFECT_CONSTANT;
    constant.magnitude=10000;
    constant.gain=128;
    constant.axisMagnitudes[0]=1;
    near(calc->calcComponentForce(&constant,calc->calcNonConditionEffectForce(&constant,0),axes,0),-5019);

    testTick=100;
    configure(hid,FFB_EFFECT_CONSTANT,100);
    calc->effects[0].magnitude=1000;
    start(hid);
    assert(status(hid)==(HID_ACTUATOR_POWER|HID_ENABLE_ACTUATORS|HID_EFFECT_PLAYING));
    testTick=125;
    hid.ffb_control(0x10);
    assert(status(hid)==(HID_ACTUATOR_POWER|HID_ENABLE_ACTUATORS|HID_EFFECT_PAUSE));
    testTick=10125;
    calc->calculateEffects(axes);
    assert(axis.getMetrics()->torque==0);
    assert(calc->effects[0].state==1);
    hid.ffb_control(0x20);
    assert(testTick-calc->effects[0].startTime==25);
    testTick+=74;
    assert(status(hid)&HID_EFFECT_PLAYING);
    testTick++;
    assert(!(status(hid)&HID_EFFECT_PLAYING));

    // Infinite start delay is consumed once, including a tick rollover.
    testTick=0xfffffff0u;
    configure(hid,FFB_EFFECT_SINE,0xffff,32);
    start(hid);
    testTick=0x0f;
    assert(!(status(hid)&HID_EFFECT_PLAYING));
    // Change configured delay/type after Start: current deadline must not move.
    configure(hid,FFB_EFFECT_TRIANGLE,0xffff,500);
    assert(calc->effects[0].startTime==0xfffffff0u);
    assert(calc->effects[0].pendingStartDelay==32);
    testTick=0x10;
    assert(status(hid)&HID_EFFECT_PLAYING);
    assert(!calc->effects[0].startDelayPending);
    testTick+=0x80000000u;
    assert(status(hid)&HID_EFFECT_PLAYING);

    // Pause a delayed effect; repeated pause does not reset pauseStartTime.
    testTick=1000;
    configure(hid,FFB_EFFECT_CONSTANT,100,100);
    start(hid);
    testTick=1020;
    hid.ffb_control(0x10);
    testTick=1100;
    hid.ffb_control(0x10);
    testTick=2020;
    hid.ffb_control(0x20);
    testTick=2099;
    assert(!(status(hid)&HID_EFFECT_PLAYING));
    testTick=2100;
    assert(status(hid)&HID_EFFECT_PLAYING);

    // Start while paused gets only its own pause duration compensated.
    hid.ffb_control(0x10);
    testTick=2200;
    start(hid);
    testTick=3200;
    hid.ffb_control(0x20);
    assert(calc->effects[0].startTime==3200);
    assert(calc->effects[0].startDelayPending);

    // Disable is distinct from Pause: clocks keep running and Set Effect/
    // Continue cannot bypass the disabled state. Enable resumes output.
    hid.ffb_control(0x02);
    configure(hid,FFB_EFFECT_CONSTANT,100);
    start(hid);
    hid.ffb_control(0x20);
    assert(!hid.getFfbActive());
    assert(status(hid)==HID_ACTUATOR_POWER);
    testTick+=101;
    hid.ffb_control(0x01);
    assert(!(status(hid)&HID_EFFECT_PLAYING));

    // Reset clears pause/effects and enables actuators; Stop All removes playing.
    hid.ffb_control(0x10);
    hid.ffb_control(0x08);
    assert(!calc->isPaused() && hid.getFfbActive());
    assert(status(hid)==(HID_ACTUATOR_POWER|HID_ENABLE_ACTUATORS));
    assert(calc->effects[0].type==FFB_EFFECT_NONE);
    configure(hid,FFB_EFFECT_CONSTANT);
    start(hid);
    hid.ffb_control(0x04);
    assert(!(status(hid)&HID_EFFECT_PLAYING));

    // Infinite waveform phase survives full HAL wrap and frequency updates.
    testTick=0xfffffff0u;
    configure(hid,FFB_EFFECT_SINE);
    FFB_SetPeriodic_Data_t periodic{};
    periodic.effectBlockIndex=1;
    periodic.period=100;
    periodic.magnitude=10000;
    hid.set_periodic(&periodic);
    start(hid);
    testTick=0x10;
    status(hid);
    assert(calc->effects[0].phaseElapsedMs==32);
    hid.ffb_control(0x10);
    testTick+=1000;
    hid.ffb_control(0x20);
    status(hid);
    assert(calc->effects[0].phaseElapsedMs==32);
    // Advance one millisecond without calculating, then update frequency.
    testTick++;
    periodic.period=200;
    hid.set_periodic(&periodic);
    assert(calc->effects[0].phaseElapsedMs==66);
    testTick++;
    hid.ffb_control(0x10);
    assert(calc->effects[0].phaseElapsedMs==67);
    periodic.period=100;
    hid.set_periodic(&periodic);
    assert(calc->effects[0].phaseElapsedMs==33);
    testTick+=1000;
    hid.ffb_control(0x20);
    status(hid);
    assert(calc->effects[0].phaseElapsedMs==33);
    periodic.phase=35999;
    hid.set_periodic(&periodic);
    assert(calc->effects[0].phase==35999);

    // Finite periodic effects preserve phase too; frequency updates must not
    // restart their duration. Both cases use the same phase accumulator.
    testTick=10000;
    configure(hid,FFB_EFFECT_SINE,1000);
    periodic.period=100;
    periodic.phase=0;
    hid.set_periodic(&periodic);
    start(hid);
    testTick+=25;
    periodic.period=200;
    hid.set_periodic(&periodic);
    assert(calc->effects[0].phaseElapsedMs==50);
    assert(calc->effects[0].startTime==10000);
    testTick=11000;
    assert(!(status(hid)&HID_EFFECT_PLAYING));

    // First velocity sample creates no inertia impulse. Following samples do.
    ODriveLocalAxis moving;
    testVel=1;
    moving.update();
    assert(moving.getMetrics()->speed==360);
    assert(moving.getMetrics()->accel==0);
    moving.update();
    assert(moving.getMetrics()->accel==0);
    testVel=2;
    moving.update();
    assert(moving.getMetrics()->accel>0);

    // Existing inversions, local endstop, EQ and clipping remain operational.
    for(int flags=0;flags<4;flags++) {
        ODriveLocalAxis local;
        local.axisInverted_=(flags&1)!=0;
        local.ffbInverted_=(flags&2)!=0;
        local.getMetrics()->posDegrees=460;
        local.calculateAxisEffects(true);
        local.setEffectTorque(0);
        assert(local.getMetrics()->torque<0);
        local.endstopStrength_=0;
        local.calculateAxisEffects(true);
        local.setEffectTorque(1000);
        assert(local.getMetrics()->torque==((flags&2)?-1000:1000));
        local.setEffectTorque(100000);
        assert(std::abs(local.getMetrics()->torque)==32767);
    }
    hid.ffb_control(0x08);
    configure(hid,FFB_EFFECT_CONSTANT);
    calc->effects[0].magnitude=1000;
    start(hid);
    calc->getEqCascade(0).setGain(EqCascade::BAND_CHASSIS,6);
    for(int i=0;i<1000;i++) { testTick++; calc->calculateEffects(axes); }
    near(axis.getMetrics()->torque,-1000); // EQ DC normalization retained.
    assert(axis.oscGuard_.cfg.strength==0); // No automatic anti-oscillation change.
    std::cout << "PASS: conditional gains, Pause/Continue, delays, states, Reset, "
                 "HAL wrap, frequency changes, first acceleration, existing inversions, endstop and EQ\n";
}
