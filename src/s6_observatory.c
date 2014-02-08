#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <malloc.h>
#include <string.h>
#include <pthread.h>
#include <fmtmsg.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/timeb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ctype.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

//extern "C"
//{
#include "mshmLib.h"
#include "execshm.h"
#include "wappshm.h"
#include "wapplib.h"
#include "alfashm.h"
#include "vtxLib.h"
#include "scram.h"

#include <hiredis.h>

//}

#include "s6obsaux.h"

// #define bool int
// #define true  1
// #define false 0
// static const double D2R=(M_PI/180.0);

int main(int argc, char ** argv) {
    struct SCRAMNET * scram;
    char    name[256];
    int     i;
    bool    got_pnt, got_agc, got_alfashm;
    time_t  time_pnt, time_agc, time_if1, time_if2, time_tt, time_alfashm, time_fix;
    double Enc2Deg = 1./ ( (4096. * 210. / 5.0) / (360.) );

    char strbuf[1024];
    redisContext *c;
    redisReply *reply;
    const char *hostname = "127.0.0.1";
    int port = 6379;

    const char *usage = "Usage: s6_observatory [-test] [-stdout] [-nodb] [-nottl] [-hostname hostname] [-port port]\n  -test: don't read scram, put in dummy values\n  -stdout: output packets to stdout (normally quiet)\n  -nodb: don't update redis db\n  -nottl: don't expire any of the scram keys in the redis db\n  hostname/port: for redis database (default 127.0.0.1:6379)\n\n";

    // int x = 0; // for debugging

    // fprintf(stderr, "scram size : %d   Enc2Deg = %20.10lf  an int is %d bytes long\n", sizeof(scram), Enc2Deg, sizeof(int));

    bool dotest = false;
    bool dostdout = false;
    bool nodb = false;
    bool nottl = false;

    double RA, Dec, MJD, azfix, zafix; // PNT vars
    int mlasttck, Az, ZA, agctime; double Azdeg, ZAdeg, timesecs, Azerrdeg, ZAerrdeg; // AGC vars
    int synIDB_0, fltrbank; double synIHz_0, rfFreq, FrqMhz; // IF1 vars
    bool useAlfa; // IF2 vars
    int encoder; double degrees; // TT vars
    int fstbias, sndbias; double motorpos; // ALFASHM vars

    double beamAz, beamZA; // ra/dec conversion per beam vars
    double coord_unixtime;
    double fixedRA[7]; // for fixed values
    double fixedDec[7];

    for (i = 1; i < argc; i++) {
      if (strcmp(argv[i],"-test") == 0) { dotest = true; }
      else if (strcmp(argv[i],"-stdout") == 0) { dostdout = true; }
      else if (strcmp(argv[i],"-nodb") == 0) { nodb = true; }
      else if (strcmp(argv[i],"-nottl") == 0) { nottl = true; }
      else if (strcmp(argv[i],"-hostname") == 0) { hostname = argv[++i]; }
      else if (strcmp(argv[i],"-port") == 0) { port = atoi(argv[++i]); }
      else { fprintf(stderr,"%s",usage); exit (1); }
      }

    if (!nodb) {
      struct timeval timeout = { 1, 500000 }; // 1.5 seconds
      c = redisConnectWithTimeout(hostname, port, timeout);
      if (c == NULL || c->err) {
          if (c) {
              printf("Connection error: %s\n", c->errstr);
              redisFree(c);
            } else {
              printf("Connection error: can't allocate redis context\n");
            }
          exit(1);
        }
      }

    if (!dotest) scram = init_scramread(NULL);

    got_pnt = got_agc = got_alfashm = false;
    time_pnt = time_agc = time_if1 = time_if2 = time_tt = time_alfashm = time_fix = 0;

    while (1) {

      if (!dotest) {

        if (read_scram(scram) == -1) {
            fprintf(stderr, "GetScramData : bad scram read\n");
            exit(1);
          }

        getnameinfo((const struct sockaddr *)&scram->from, sizeof(struct sockaddr_in), name, 256, NULL, 0, 0);

        if (strcmp(scram->in.magic, "PNT") == 0) {
            got_pnt = true;
            time_pnt = time(NULL);
            RA  = scram->pntData.st.x.pl.curP.raJ;
            Dec = scram->pntData.st.x.pl.curP.decJ;
            RA  *= 24.0 / C_2PI;
            Dec *= 360.0 / C_2PI;
            MJD  = scram->pntData.st.x.pl.tm.mjd + scram->pntData.st.x.pl.tm.ut1Frac;
            azfix = scram->pntData.st.x.modelCorEncAzZa[0];
            zafix = scram->pntData.st.x.modelCorEncAzZa[1];
            sprintf(strbuf,"SCRAM:PNT PNTSTIME %ld PNTRA %0.10lf PNTDEC %0.10lf PNTMJD %0.10lf PNTAZCOR %0.10lf PNTZACOR %0.10lf",time_pnt,RA,Dec,MJD,azfix,zafix);
            if (!nodb) { reply = redisCommand(c,"HMSET %s",strbuf); freeReplyObject(reply); }
            if (dostdout) fprintf(stderr,"%s\n",strbuf);
          } else if (strcmp(scram->in.magic, "AGC") == 0) {
            got_agc = true;
            time_agc = time(NULL);
            mlasttck = scram->agcData.st.secMLastTick;
            Az = scram->agcData.st.cblkMCur.dat.posAz;
            Azdeg = scram->agcData.st.cblkMCur.dat.posAz * 0.0001;
            ZA = scram->agcData.st.cblkMCur.dat.posGr; 
            ZAdeg = scram->agcData.st.cblkMCur.dat.posGr * 0.0001;
            agctime = scram->agcData.st.cblkMCur.dat.timeMs;
            timesecs = scram->agcData.st.cblkMCur.dat.timeMs * 0.001; 
            Azerrdeg = scram->agcData.st.posErr.reqPosDifRd[0];
            ZAerrdeg = scram->agcData.st.posErr.reqPosDifRd[1];
            // WHAT IS AGCLST?!?!?!! TODO
            sprintf(strbuf,"SCRAM:AGC AGCSTIME %ld AGCTIME %d AGCAZ %0.10lf AGCZA %0.10lf AGCLST %0.10lf",time_agc,agctime,Azdeg,ZAdeg,0.0);
            if (!nodb) { reply = redisCommand(c,"HMSET %s",strbuf); freeReplyObject(reply); }
            if (dostdout) fprintf(stderr,"%s\n",strbuf);
          } else if (strcmp(scram->in.magic, "IF1") == 0) {
            time_if1 = time(NULL); 
            synIHz_0 = scram->if1Data.st.synI.freqHz[0];
            synIDB_0 = scram->if1Data.st.synI.ampDb[0];
            rfFreq = scram->if1Data.st.rfFreq;
            FrqMhz = scram->if1Data.st.if1FrqMhz;
            fltrbank = scram->if1Data.st.stat2.alfaFb;
            sprintf(strbuf,"SCRAM:IF1 IF1STIME %ld IF1SYNHZ %0.10lf IF1SYNDB %d IF1RFFRQ %0.10lf IF1IFFRQ %0.10lf IF1ALFFB %d",time_if1,synIHz_0,synIDB_0,rfFreq,FrqMhz,fltrbank);
            if (!nodb) { reply = redisCommand(c,"HMSET %s",strbuf); freeReplyObject(reply); }
            if (dostdout) fprintf(stderr,"%s\n",strbuf);
          } else if (strcmp(scram->in.magic, "IF2") == 0) {
            time_if2 = time(NULL); 
            if(scram->if2Data.st.stat1.useAlfa) { useAlfa = true; } else { useAlfa = false; }
            sprintf(strbuf,"SCRAM:IF2 IF2STIME %ld IF2ALFON %d",time_if2,useAlfa);
            if (!nodb) { reply = redisCommand(c,"HMSET %s",strbuf); freeReplyObject(reply); }
            if (dostdout) fprintf(stderr,"%s\n",strbuf);
          } else if (strcmp(scram->in.magic, "TT") == 0) {
            time_tt = time(NULL);
            encoder = scram->ttData.st.slv[0].inpMsg.position;
            degrees = (double)(scram->ttData.st.slv[0].inpMsg.position) * Enc2Deg;
            sprintf(strbuf,"SCRAM:TT TTSTIME %ld TTTURENC %d TTTURDEG %0.10lf",time_tt,encoder,degrees);
            if (!nodb) { reply = redisCommand(c,"HMSET %s",strbuf); freeReplyObject(reply); }
            if (dostdout) fprintf(stderr,"%s\n",strbuf);
          } else if (strcmp(scram->in.magic, "ALFASHM") == 0) {
            got_alfashm = true; 
            time_alfashm = time(NULL);
            fstbias = (int)scram->alfa.first_bias;
            sndbias = (int)scram->alfa.second_bias;
            motorpos = scram->alfa.motor_position; 
            sprintf(strbuf,"SCRAM:ALFASHM ALFSTIME %ld ALFBIAS1 %d ALFBIAS2 %d ALFMOPOS %0.10lf",time_alfashm,fstbias,sndbias,motorpos);
            if (!nodb) { reply = redisCommand(c,"HMSET %s",strbuf); freeReplyObject(reply); }
            if (dostdout) fprintf(stderr,"%s\n",strbuf);
          } else {
            // fprintf(stderr, "UNKNOWN SCRAM: %ld %s from %s\n", current, scram->in.magic, name);
          }

        if (got_alfashm && got_agc && got_pnt) {
          time_fix = time_alfashm;
          if (time_agc > time_fix) time_fix = time_agc;
          if (time_pnt > time_fix) time_fix = time_pnt;
          coord_unixtime = s6_seti_ao_timeMS2unixtime(agctime,time_fix);
          fprintf(stderr,"agctime %d time_fix %ld coord_unixtime %lf\n",agctime,time_fix,coord_unixtime);
          for (i=0;i<7;i++) {
            beamAz = Azdeg; beamZA = ZAdeg; // in degrees
            fprintf(stderr,"i %d Az %lf ZA %lf ",i,beamAz,beamZA);
            beamAz -= azfix; beamZA -= zafix; // fix also in degrees
            fprintf(stderr,"fixed: Az %lf ZA %lf ",beamAz,beamZA);
            s6_BeamOffset(&beamAz, &beamZA, i, motorpos); // i is beam
            fprintf(stderr,"post offset: Az %lf ZA %lf \n",beamAz,beamZA);
            s6_AzZaToRaDec(beamAz, beamZA, coord_unixtime, &fixedRA[i], &fixedDec[i]);
            }
          sprintf(strbuf,"SCRAM:DERIVED DERTIME %ld RA0 %0.10lf DEC0 %0.10lf RA1 %0.10lf DEC1 %0.10lf RA2 %0.10lf DEC2 %0.10lf RA3 %0.10lf DEC3 %0.10lf RA4 %0.10lf DEC4 %0.10lf RA5 %0.10lf DEC5 %0.10lf RA6 %0.10lf DEC6 %0.10lf",time_fix,fixedRA[0],fixedDec[0],fixedRA[1],fixedDec[1],fixedRA[2],fixedDec[2],fixedRA[3],fixedDec[3],fixedRA[4],fixedDec[4],fixedRA[5],fixedDec[5],fixedRA[6],fixedDec[6]);
          if (!nodb) { reply = redisCommand(c,"HMSET %s",strbuf); freeReplyObject(reply); }
          if (dostdout) fprintf(stderr,"%s\n",strbuf);
          }
  
        } // end if !nodotest

      else { // test mode

        fprintf(stderr, ".");  // indicate that we are running

        sprintf(strbuf,"SCRAM:PNT PNTSTIME \"%ld\" PNTRA \"11.1\" PNTDEC \"2.2\" PNTMJD \"3.3\" PNTAZCOR \"4.4\" PNTZACOR \"5.5\"",time(NULL));
        if (dostdout) fprintf(stderr,"%s\n",strbuf);
        if (!nodb) { 
            reply = redisCommand(c,"HMSET %s",strbuf); 
            if (reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "Error: %s\n", reply->str);
            } else if (reply->type != REDIS_REPLY_ARRAY ) {
                fprintf(stderr, "Unexpected type: %d\n", reply->type);
            }
            freeReplyObject(reply);
        }

        sprintf(strbuf,"SCRAM:AGC AGCSTIME %ld AGCTIME 1 AGCAZ 2.2 AGCZA 3.3 AGCLST 4.4",time(NULL));
        if (dostdout) fprintf(stderr,"%s\n",strbuf);
        if (!nodb) { 
            reply = redisCommand(c,"HMSET %s",strbuf); 
            if (reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "Error: %s\n", reply->str);
            } else if (reply->type != REDIS_REPLY_ARRAY ) {
                fprintf(stderr, "Unexpected type: %d\n", reply->type);
            }
            freeReplyObject(reply);
        }
        sprintf(strbuf,"SCRAM:IF1 IF1STIME %ld IF1SYNHZ 1.1 IF1SYNDB 2 IF1RFFRQ 3.3 IF1IFFRQ 4.4 IF1ALFFB 5",time(NULL));
        if (dostdout) fprintf(stderr,"%s\n",strbuf);
        if (!nodb) { 
            reply = redisCommand(c,"HMSET %s",strbuf); 
            if (reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "Error: %s\n", reply->str);
            } else if (reply->type != REDIS_REPLY_ARRAY ) {
                fprintf(stderr, "Unexpected type: %d\n", reply->type);
            }
            freeReplyObject(reply);
        }

        sprintf(strbuf,"SCRAM:IF2 IF2STIME %ld IF2ALFON 1",time(NULL));
        if (dostdout) fprintf(stderr,"%s\n",strbuf);
        if (!nodb) { 
            reply = redisCommand(c,"HMSET %s",strbuf); 
            if (reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "Error: %s\n", reply->str);
            } else if (reply->type != REDIS_REPLY_ARRAY ) {
                fprintf(stderr, "Unexpected type: %d\n", reply->type);
            }
            freeReplyObject(reply);
        }

        sprintf(strbuf,"SCRAM:TT TTSTIME %ld TTTURENC 1 TTTURDEG 2.2",time(NULL));
        if (dostdout) fprintf(stderr,"%s\n",strbuf);
        if (!nodb) { 
            reply = redisCommand(c,"HMSET %s",strbuf); 
            if (reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "Error: %s\n", reply->str);
            } else if (reply->type != REDIS_REPLY_ARRAY ) {
                fprintf(stderr, "Unexpected type: %d\n", reply->type);
            }
            freeReplyObject(reply);
        }

        sprintf(strbuf,"SCRAM:ALFASHM ALFSTIME %ld ALFBIAS1 1 ALFBIAS2 2 ALFMOPOS 3.3",time(NULL));
        if (dostdout) fprintf(stderr,"%s\n",strbuf);
        if (!nodb) { 
            reply = redisCommand(c,"HMSET %s",strbuf); 
            if (reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "Error: %s\n", reply->str);
            } else if (reply->type != REDIS_REPLY_ARRAY ) {
                fprintf(stderr, "Unexpected type: %d\n", reply->type);
            }
            freeReplyObject(reply);
        }

        sprintf(strbuf,"SCRAM:DERIVED DERTIME %ld RA0 0.0 DEC0 0.0 RA1 1.0 DEC1 1.0 RA2 2.0 DEC2 2.0 RA3 3.0 DEC3 3.0 RA4 4.0 DEC4 4.0 RA5 5.0 DEC5 5.0 RA6 6.0 DEC6 6.0",time(NULL));
        if (dostdout) fprintf(stderr,"%s\n",strbuf);
        if (!nodb) { 
            reply = redisCommand(c,"HMSET %s",strbuf); 
            if (reply->type == REDIS_REPLY_ERROR) {
                fprintf(stderr, "Error: %s\n", reply->str);
            } else if (reply->type != REDIS_REPLY_ARRAY ) {
                fprintf(stderr, "Unexpected type: %d\n", reply->type);
            }
            freeReplyObject(reply);
        }
        
        sleep(1); // just so we make it look more like scram and don't hammer redis db

        } // end if dotest

      } // end while (1)

    exit(0);
}
