/*
  uart is for trace output.
  uart1 should be connected to the GPS device.
*/

#include "Cosa/RTC.hh"
#include "Cosa/Trace.hh"
#include "Cosa/IOBuffer.hh"
#include "Cosa/IOStream/Driver/UART.hh"
#include "Cosa/Watchdog.hh"

#include "NMEAGPS.h"

//  The NMEAGPS member is hooked directly to the UART so that it processes
//  characters in the interrupt.
//  The time window for accessing a coherent /fix/ is fairly narrow, 
//  about 9 character times, or about 10mS on a 9600-baud connection.

class MyGPS : public IOStream::Device
{
protected:
    NMEAGPS gps;

public:
    NMEAGPS::gps_fix_t merged;
    volatile bool frame_received;

    /**
     * Constructor
     */
    MyGPS( IOStream::Device *device = (IOStream::Device *) NULL )
      : gps( device ),
        frame_received( false )
        {}

    /**
     * Written to by UART driver as soon as a new char is received.
     * Called inside Irq handler.
     */
    virtual int putchar( char c )
    {
      if (gps.decode(c))
        frame_received = true;

      return c;
    };

    const volatile NMEAGPS::gps_fix_t & fix() const { return gps.fix(); };

    bool is_coherent() const { return gps.is_coherent(); }
    void poll( NMEAGPS::nmea_msg_t msg ) const { gps.poll(msg); };
    void send( const char *msg ) const { gps.send(msg); };
    void send_P( const char *msg ) const { gps.send_P(msg); };
};

extern UART uart1;
static MyGPS gps( &uart1 );

static IOBuffer<UART::BUFFER_MAX> obuf;
UART uart1(1, &gps, &obuf);

static clock_t now = 0;
static clock_t lastTrace = 0;

//--------------------------

//#define USE_FLOAT

static void traceIt()
{
  if (gps.merged.valid.dateTime) {
    trace << gps.merged.dateTime << PSTR(".");
    if (gps.merged.dateTime_cs < 10)
      trace << '0';
    trace << gps.merged.dateTime_cs;
  } else {
    //  Apparently we don't have a fix yet, ask for a ZDA (Zulu Date and Time)
    gps.poll( NMEAGPS::NMEA_ZDA );
  }
  trace << PSTR(",");

#ifdef USE_FLOAT
  trace.width(3);
  trace.precision(6);
  if (gps.merged.valid.location)
    trace << gps.merged.latitude() << PSTR(",") << gps.merged.longitude();
  else
    trace << PSTR(",");
  trace << PSTR(",");
  trace.precision(2);
  if (gps.merged.valid.heading)
    trace << gps.merged.heading();
  trace << PSTR(",");
  trace.precision(3);
  if (gps.merged.valid.speed)
    trace << gps.merged.speed();
  trace << PSTR(",");
  trace.precision(2);
  if (gps.merged.valid.altitude)
    trace << gps.merged.altitude();
#else
  if (gps.merged.valid.location)
    trace << gps.merged.latitudeL() << PSTR(",") << gps.merged.longitudeL();
  else
    trace << PSTR(",");
  trace << PSTR(",");
  if (gps.merged.valid.heading)
    trace << gps.merged.heading_cd();
  trace << PSTR(",");
  if (gps.merged.valid.speed)
    trace << gps.merged.speed_mkn();
  trace << PSTR(",");
  if (gps.merged.valid.altitude)
    trace << gps.merged.altitude_cm();
#endif
  trace << endl;

  lastTrace = now;

} // traceIt

//--------------------------

void setup()
{
  // Watchdog for sleeping
  Watchdog::begin( 16, Watchdog::push_timeout_events );
  RTC::begin();

  // Start the normal trace output
  uart.begin(9600);
  trace.begin(&uart, PSTR("CosaGPSDevice: started"));
  trace << PSTR("fix object size = ") << sizeof(gps.fix()) << endl;
  trace << PSTR("GPS object size = ") << sizeof(gps) << endl;
  uart.flush();

  // Start the UART for the GPS device
  uart1.begin(9600);
}

//--------------------------

void loop()
{
  bool new_fix = false;
  bool new_safe_fix = false;
  NMEAGPS::gps_fix_t safe_fix;


  synchronized {
    if (gps.frame_received) {
      gps.frame_received = false;

      // This can be susceptible to processing delays; missing an /is_coherent/
      // means that some data may not get copied into safe_fix.
      if (gps.is_coherent()) {
        if (gps.fix().valid.dateTime && gps.merged.valid.dateTime &&
            (gps.merged.dateTime != *const_cast<const time_t *>(&gps.fix().dateTime)))
          new_fix = true;
        safe_fix = *const_cast<const NMEAGPS::gps_fix_t *>(&gps.fix());
        new_safe_fix = true;
      }
    }
  }

  if (new_fix) {
    traceIt();
    gps.merged = safe_fix;
  } else if (new_safe_fix)
    gps.merged |= safe_fix;

  now = RTC::time();

  if (lastTrace + 5 <= now)
    traceIt();
}