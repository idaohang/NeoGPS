/**
 * @file NMEAGPS.cpp
 * @version 2.1
 *
 * @section License
 * Copyright (C) 2014, SlashDevin
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "NMEAGPS.h"

#include "Cosa/IOStream.hh"
#include "Cosa/Trace.hh"

#ifndef CR
#define CR (13)
#endif
#ifndef LF
#define LF (10)
#endif

/**
 * parseHEX(char a)
 * Parses a single character as HEX and returns byte value.
 */
inline static uint8_t parseHEX(char a) {
    a |= 0x20; // make it lowercase
    if (('a' <= a) && (a <= 'f'))
        return a - 'a' + 10;
    else
        return a - '0';
}

static char toHexDigit( uint8_t val )
{
  val &= 0x0F;
  return (val >= 10) ? ((val - 10) + 'A') : (val + '0');
}


void NMEAGPS::rxBegin()
{
    crc = 0;
    nmeaMessage = NMEA_UNKNOWN;
    rxState = NMEA_RECEIVING_DATA;
    fieldIndex = 0;
    chrCount = 0;
}


void NMEAGPS::rxEnd( bool ok )
{
  rxState = NMEA_IDLE;

  if (ok) {
    coherent = true;

    if (m_fix.status == gps_fix::STATUS_NONE)
      m_fix.valid.as_byte = 0;
    else if (m_fix.status == gps_fix::STATUS_TIME_ONLY) {
#ifdef GPS_FIX_DATE
      bool dateValid = m_fix.valid.date;
#endif
#ifdef GPS_FIX_TIME
      bool timeValid = m_fix.valid.time;
#endif
#if defined(GPS_FIX_DATE) | defined(GPS_FIX_TIME)
      m_fix.valid.as_byte = 0; // nothing else is valid
#endif
#ifdef GPS_FIX_DATE
      m_fix.valid.date = dateValid;
#endif
#ifdef GPS_FIX_TIME
      m_fix.valid.time = timeValid;
#endif
    }

  } else {
    m_fix.valid.as_byte = 0;
    nmeaMessage = NMEA_UNKNOWN;
  }

#ifdef NMEAGPS_STATS
  if (ok)
    statistics.parser_ok++;
#endif
}


NMEAGPS::decode_t NMEAGPS::decode( char c )
{
  decode_t res = DECODE_CHR_OK;

  if (c == '$') {  // Always restarts
    rxBegin();

  } else {
    switch (rxState) {
      case NMEA_IDLE:
          res = DECODE_CHR_INVALID;
          nmeaMessage = NMEA_UNKNOWN;
//trace << 'X' << toHexDigit(c >> 4) << toHexDigit(c);
          break;

          // Wait until complete line is received
      case NMEA_RECEIVING_DATA:
          if (c == '*') {   // Line finished, CRC follows
              rxState = NMEA_RECEIVING_CRC1;

          } else if ((c == CR) || (c == LF)) { // Line finished, no CRC
              rxEnd( true );
              res = DECODE_COMPLETED;

          } else if ((c < ' ') || ('~' < c)) { // Invalid char
              res = DECODE_CHR_INVALID;
              rxEnd( false );

          } else {            // normal data character
              crc ^= c;

              if (fieldIndex == 0) {
                decode_t cmd_res = parseCommand( c );
                if (cmd_res == DECODE_COMPLETED) {
                  m_fix.valid.as_byte = 0;
                  coherent = false;
                } else if (cmd_res == DECODE_CHR_INVALID) {
                  rxEnd( false );
                }

              } else if (!parseField(c))
//{ trace << PSTR("!pf @ ") << nmeaMessage << PSTR(":") << fieldIndex << PSTR("/") << chrCount << PSTR("'") << c << PSTR("'\n");
                rxEnd( false );
//}

              if (c == ',') { // a comma marks the next field
                fieldIndex++;
                chrCount = 0;
              } else
                chrCount++;
          }
          break;
          
          
          // Receiving first CRC character
      case NMEA_RECEIVING_CRC1:
          if (crc>>4 != parseHEX(c)) { // mismatch, count as CRC error
#ifdef NMEAGPS_STATS
              statistics.parser_crcerr++;
#endif
              rxEnd( false );
          } else  // valid first CRC nibble
              rxState = NMEA_RECEIVING_CRC2;
          break;
          
          
          // Receiving second CRC character, parse line if CRC matches
      case NMEA_RECEIVING_CRC2:
          if ((crc & 0x0F) != parseHEX(c)) {// CRCs do not match
#ifdef NMEAGPS_STATS
              statistics.parser_crcerr++;
#endif
              rxEnd( false );
          } else { // valid second CRC nibble
              rxEnd( true );
              res = DECODE_COMPLETED;
          }
          break;
    }
  }

  return res;
}

static const char gpgga[] __PROGMEM =  "GPGGA";
static const char gpgll[] __PROGMEM =  "GPGLL";
static const char gpgsa[] __PROGMEM =  "GPGSA";
static const char gpgsv[] __PROGMEM =  "GPGSV";
static const char gprmc[] __PROGMEM =  "GPRMC";
static const char gpvtg[] __PROGMEM =  "GPVTG";
static const char gpzda[] __PROGMEM =  "GPZDA";

const char * const NMEAGPS::std_nmea[] __PROGMEM = {
  gpgga,
  gpgll,
  gpgsa,
  gpgsv,
  gprmc,
  gpvtg,
  gpzda
};

const uint8_t NMEAGPS::std_nmea_size = membersof(std_nmea);

const NMEAGPS::msg_table_t NMEAGPS::nmea_msg_table __PROGMEM =
  {
    NMEAGPS::NMEA_FIRST_MSG,
    (const msg_table_t *) NULL,
    NMEAGPS::std_nmea_size,
    NMEAGPS::std_nmea
  };

NMEAGPS::decode_t NMEAGPS::parseCommand( char c )
{
//trace << c;
  const msg_table_t *msgs = msg_table();

  for (;;) {
    uint8_t  table_size       = pgm_read_byte( &msgs->size );
    uint8_t  msg_offset       = pgm_read_byte( &msgs->offset );
    decode_t res              = DECODE_CHR_INVALID;
    bool     check_this_table = true;
    uint8_t  entry;

    if (nmeaMessage == NMEA_UNKNOWN)
      entry = 0;
    else if ((msg_offset <= nmeaMessage) && (nmeaMessage < msg_offset+table_size))
      entry = nmeaMessage - msg_offset;
    else
      check_this_table = false;

    if (check_this_table) {
      uint8_t i = entry;

      const char * const *table   = (const char * const *) pgm_read_word( &msgs->table );
      const char *        table_i = (const char *) pgm_read_word( &table[i] );
      
      for (;;) {
        char rc = pgm_read_byte( &table_i[chrCount] );
        if (c == rc) {
          entry = i;
          res = DECODE_CHR_OK;
          break;
        }
        if ((c == ',') && (rc == 0)) {
//trace << PSTR(" -> ") << (uint8_t)entry << endl;
          res = DECODE_COMPLETED;
          break;
        }
        uint8_t next_msg = i+1;
        if (next_msg >= table_size) {
//trace << PSTR(" -> UNK (no more entries)\n");
          break;
        }
        const char *table_next = (const char *) pgm_read_word( &table[next_msg] );
        for (uint8_t j = 0; j < chrCount; j++)
          if (pgm_read_byte( &table_i[j] ) != pgm_read_byte( &table_next[j] )) {
//trace << PSTR(" -> UNK (next entries have different start)\n");
            break;
          }
        i = next_msg;
        table_i = table_next;
      }
    }

    if (res == DECODE_CHR_INVALID) {
      msgs = (const msg_table_t *) pgm_read_word( &msgs->previous );
      if (msgs)
//{ trace << '^'; // << hex << msgs << '=' << hex << &nmea_msg_table << ' ';
        continue;
//}
    } else
      nmeaMessage = (nmea_msg_t) (entry + msg_offset);

    return res;
  }
}

//---------------------------------------------

bool NMEAGPS::parseField(char chr)
{
    bool ok = true;
    switch (nmeaMessage) {

        case NMEA_GGA:
#ifdef NMEAGPS_PARSE_GGA
          switch (fieldIndex) {
              CASE_TIME(1);
              CASE_LOC(2);
              CASE_FIX(6);
              CASE_SAT(7);
              CASE_HDOP(8);
              CASE_ALT(9);
          }
#endif
          break;

        case NMEA_GLL:
#ifdef NMEAGPS_PARSE_GLL
          switch (fieldIndex) {
              CASE_LOC(1);
              CASE_TIME(5);
//            case 6:  duplicate info
              CASE_FIX(7);
          }
#endif
          break;

        case NMEA_GSA:
        case NMEA_GSV:
            break;
                  
        case NMEA_RMC:
#ifdef NMEAGPS_PARSE_RMC
          switch (fieldIndex) {
              CASE_TIME(1);
              CASE_FIX(2);
              CASE_LOC(3);
              CASE_SPEED(7);
              CASE_HEADING(8);
              CASE_DATE(9);
              CASE_FIX(12);
          }
#endif
          break;

        case NMEA_VTG:
#ifdef NMEAGPS_PARSE_VTG
          switch (fieldIndex) {
              CASE_HEADING(1);
              CASE_SPEED(5);
              CASE_FIX(9);
          }
#endif
          break;

        case NMEA_ZDA:
#ifdef NMEAGPS_PARSE_ZDA
          switch (fieldIndex) {
              CASE_TIME(1);
#ifdef GPS_FIX_DATE
              case 2:                         // Date
                  if (chrCount == 0)
                    m_fix.dateTime.date  = 0;
                  if (chr != ',')
                    m_fix.dateTime.date  = (m_fix.dateTime.date *10) + (chr - '0');
                  break;
              case 3:                         // Month
                  if (chrCount == 0)
                    m_fix.dateTime.month = 0;
                  if (chr != ',')
                    m_fix.dateTime.month = (m_fix.dateTime.month*10) + (chr - '0');
                  break;
              case 4:                         // Year
                  if (chrCount == 0)
                    m_fix.dateTime.year  = 0;
                  if ((2 <= chrCount) && (chrCount <= 3))
                    m_fix.dateTime.year  = (m_fix.dateTime.year *10) + (chr - '0');
                  else if (chr == ',')
                    m_fix.valid.date = true;
                  break;
#endif
          }
#endif
          break;

        default:
            ok = false;
            break;
    }

    return ok;
}

#ifdef GPS_FIX_TIME

bool NMEAGPS::parseTime(char chr)
{
  bool ok = true;

  switch (chrCount) {
      case 0: m_fix.dateTime.hours    = (chr - '0')*10; break;
      case 1: m_fix.dateTime.hours   += (chr - '0');    break;
      case 2: m_fix.dateTime.minutes  = (chr - '0')*10; break;
      case 3: m_fix.dateTime.minutes += (chr - '0');    break;
      case 4: m_fix.dateTime.seconds  = (chr - '0')*10; break;
      case 5: m_fix.dateTime.seconds += (chr - '0');    break;
      case 6: if (chr != '.') ok = false;               break;
      case 7: m_fix.dateTime_cs       = (chr - '0')*10; break;
      case 8: m_fix.dateTime_cs      += (chr - '0');    break;
      case 9:
        if (chr == ',')
          m_fix.valid.time = true;
        else
          ok = false;
        break;
  }

  return ok;
}
#endif

#ifdef GPS_FIX_DATE

bool NMEAGPS::parseDDMMYY( char chr )
{
  bool ok = true;
  switch (chrCount) {
    case 0: m_fix.dateTime.date   = (chr - '0')*10; break;
    case 1: m_fix.dateTime.date  += (chr - '0');    break;
    case 2: m_fix.dateTime.month  = (chr - '0')*10; break;
    case 3: m_fix.dateTime.month += (chr - '0');    break;
    case 4: m_fix.dateTime.year   = (chr - '0')*10; break;
    case 5: m_fix.dateTime.year  += (chr - '0');    break;
    case 6:
      if (chr == ',')
        m_fix.valid.date = true;
      else
        ok = false;
      break;
  }
  return ok;
}

#endif

bool NMEAGPS::parseFix( char chr )
{
  bool ok = true;

  if (chrCount == 0) {
    if ((chr == '1') || (chr == 'A'))
      m_fix.status = gps_fix::STATUS_STD;
    else if ((chr == '0') || (chr == 'N') || (chr == 'V'))
      m_fix.status = gps_fix::STATUS_NONE;
    else if ((chr == '2') || (chr == 'D'))
      m_fix.status = gps_fix::STATUS_DGPS;
    else if ((chr == '6') || (chr == 'E'))
      m_fix.status = gps_fix::STATUS_EST;
    else
      ok = false;
  } else if ((chrCount == 1) && (chr != ',')) {
    ok = false;
  }

  return ok;
}

bool NMEAGPS::parseFloat( gps_fix::whole_frac & val, char chr, uint8_t max_decimal )
{
  if (chrCount == 0) {
    val.init();
    decimal = 0;
    negative = (chr == '-');
    if (negative) return true;
  }

  if (chr == ',') {
    // End of field, make sure it's scaled up
    if (!decimal)
      decimal = 1;
    while (decimal++ <= max_decimal)
      val.frac *= 10;
    if (negative) {
      val.frac = -val.frac;
      val.whole = -val.whole;
    }
  } else if (chr == '.') {
    decimal = 1;
  } else if (!decimal) {
    val.whole = val.whole*10 + (chr - '0');
  } else if (decimal++ <= max_decimal) {
    val.frac = val.frac*10 + (chr - '0');
  }
  return true;
}

#ifdef GPS_FIX_LOCATION

bool NMEAGPS::parseDDDMM( int32_t & val, char chr )
{
  // parse lat/lon dddmm.mmmm fields

  if (chrCount == 0) {
    val = 0;
    decimal = 0;
  }
  
  if ((chr == '.') || ((chr == ',') && !decimal)) {
    // Now we know how many digits are in degrees; all but the last two.
    // Switch from BCD (digits) to binary minutes.
    decimal = 1;
    uint8_t *valBCD = (uint8_t *) &val;
    uint8_t  deg     = to_binary( valBCD[1] );
    if (valBCD[2] != 0)
      deg += 100; // only possible if abs(longitude) >= 100.0 degrees
    val = (deg * 60) + to_binary( valBCD[0] );
    // val now in units of minutes
    if (chr == '.') return true;
  }
  
  if (chr == ',') {
    if (val) {
      // If the last chars in ".mmmm" were not received,
      //    force the value into its final state.
      while (decimal++ < 6)
        val *= 10;
      // Value was in minutes x 1000000, convert to degrees x 10000000.
      val += (val*2 + 1)/3; // aka (100*val+30)/60, but without sign truncation
    }
  } else if (!decimal) {
    // val is BCD until *after* decimal point
    val = (val<<4) | (chr - '0');
  } else if (decimal++ < 6) {
    val = val*10 + (chr - '0');
  }
  return true;
}
#endif

void NMEAGPS::poll( IOStream::Device *device, nmea_msg_t msg )
{
  //  Only the ublox documentation references talker ID "EI".  
  //  Other manufacturer's devices use "II" and "GP" talker IDs for the GPQ sentence.
  //  However, "GP" is reserved for the GPS device, so it seems inconsistent
  //  to use that talker ID when requesting something from the GPS device.
  static const char pm0[] __PROGMEM = "EIGPQ,GGA";
  static const char pm1[] __PROGMEM = "EIGPQ,GLL";
  static const char pm2[] __PROGMEM = "EIGPQ,GSA";
  static const char pm3[] __PROGMEM = "EIGPQ,GSV";
  static const char pm4[] __PROGMEM = "EIGPQ,RMC";
  static const char pm5[] __PROGMEM = "EIGPQ,VTG";
  static const char pm6[] __PROGMEM = "EIGPQ,ZDA";
  static const char * const poll_msgs[] __PROGMEM = { pm0, pm1, pm2, pm3, pm4, pm5, pm6 };

  if ((NMEA_FIRST_MSG <= msg) && (msg <= NMEA_LAST_MSG))
    send( device, (str_P) pgm_read_word(&poll_msgs[msg-NMEA_FIRST_MSG]) );
}




static void send_trailer( IOStream::Device *device, uint8_t crc )
{
  device->putchar('*');

  char hexDigit = toHexDigit( crc>>4 );
  device->putchar( hexDigit );

  hexDigit = toHexDigit( crc );
  device->putchar( hexDigit );

  device->putchar( CR );
  device->putchar( LF );
}


void NMEAGPS::send( IOStream::Device *device, const char *msg )
{
  if (msg && *msg) {
    device->putchar('$');
    if (*msg == '$')
      msg++;
    uint8_t crc = 0;
    while (*msg) {
      crc ^= *msg;
      device->putchar( *msg++ );
    }

    send_trailer( device, crc );
  }
}

void NMEAGPS::send( IOStream::Device *device, str_P msg )
{
//trace << PSTR("NMEAGPS::send \"");
  if (msg) {
    uint8_t crc = 0;
    const char *ptr = (const char *)msg;
    char chr = pgm_read_byte(ptr++);
    if (chr && (chr != '$'))
//{ trace << '$';
      device->putchar('$');
//}
    while (chr) {
      crc ^= chr;
//trace << chr;
      device->putchar( chr );
      chr = pgm_read_byte(ptr++);
    }

    send_trailer( device, crc );
  }
//trace << PSTR("\"\n");
}
