#ifndef RT1180_ATS_TRACING_HELPER
#ifdef CONFIG_PERCEPIO_TRACERECORDER
extern TraceStringHandle_t xChannel;
extern TraceStringHandle_t xUpdate;
extern TraceStringHandle_t xSend;
extern TraceStringHandle_t xCb;
extern TraceStringHandle_t xTx;
extern TraceStringHandle_t xAlarm;

#define ats_trace_vars            \
    TraceStringHandle_t xChannel; \
    TraceStringHandle_t xUpdate;  \
    TraceStringHandle_t xSend;    \
    TraceStringHandle_t xCb;      \
    TraceStringHandle_t xTx;      \
    TraceStringHandle_t xAlarm;
#define ats_trace_init(...)                               \
    xTraceStringRegister("ats", &xChannel);               \
    xTraceStringRegister("update: %d, %d, %d", &xUpdate); \
    xTraceStringRegister("send: %d, %d", &xSend);         \
    xTraceStringRegister("cb: %d", &xCb);                 \
    xTraceStringRegister("tx: %d", &xTx);                 \
    xTraceStringRegister("alarm: %d, %d", &xAlarm);

#define ats_trace_counter_update_ats_time(offset, setpoint) xTracePrintF2(xChannel, xAlarm, offset, setpoint)
#define ats_trace_counter_update(nexEleg, timeUntilEleg, setRes) xTracePrintF3(xChannel, xUpdate, nexEleg, timeUntilEleg, setRes)
#define ats_trace_new_pkt(sId, pktId) xTracePrintF2(xChannel, xSend, sId, pktId)
#define ats_trace_alarm_cb(cbTicks) xTracePrintF1(xChannel, xCb, cbTicks)
#define ats_trace_transmit(pktId) xTracePrintF1(xChannel, xTx, pktId)

#else

#define ats_trace_vars
#define ats_trace_init(...)
#define ats_trace_counter_update_ats_time(...)
#define ats_trace_counter_update(...)
#define ats_trace_new_pkt(...)
#define ats_trace_alarm_cb(...)
#define ats_trace_transmit(...)

#endif
#endif