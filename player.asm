* QLVGM position-independent QDOS/QSound2 player.
* Generated constants and a QLZ1 stream are added by the host utility.

QSOUND2_ADDRESS EQU $0C3000
QLZ_MAGIC       EQU $514C5A31
QLZ_HEADER_SIZE EQU 12
STATE_STREAM    EQU 0
STATE_END       EQU 4
STATE_LOOP      EQU 8
STATE_FINISHED  EQU 12
STATE_TEST      EQU 13

start:
        BRA.W   player_start

player_start:
        LEA     start(PC),A4
        LEA     qlz_data(PC),A0
        MOVEA.L A4,A1
        ADDA.L  #loaded_end-start,A1
        MOVEA.L A1,A2
        MOVEA.L A1,A3
        ADDA.L  #DECODED_SIZE,A3
        BSR.W   qlz_decompress
        TST.L   D0
        BNE.W   player_decode_failed
        MOVEA.L A1,A5
        SUBA.L  #DECODED_SIZE,A5

        LEA     player_state(PC),A3
        MOVE.L  A5,STATE_STREAM(A3)
        MOVE.L  A1,STATE_END(A3)
        CLR.L   STATE_LOOP(A3)
        CLR.B   STATE_FINISHED(A3)
        CLR.B   STATE_TEST(A3)
        MOVE.L  #LOOP_OFFSET,D0
        CMPI.L  #-1,D0
        BEQ.S   player_no_loop
        MOVEA.L A5,A2
        ADDA.L  D0,A2
        MOVE.L  A2,STATE_LOOP(A3)
player_no_loop:
        BSR.W   qsound_reset

player_loop:
        MOVEQ   #-1,D1
        MOVEQ   #1,D3
        SUBA.L  A1,A1
        MOVEQ   #8,D0
        TRAP    #1
        BSR.W   play_frame
        MOVE.W  #TEST_FRAMES,D0
        BEQ.S   player_loop
        LEA     player_state(PC),A3
        ADDQ.B  #1,STATE_TEST(A3)
        CMP.B   STATE_TEST(A3),D0
        BNE.S   player_loop
        SUBA.L  A0,A0
        JMP     (A0)

player_decode_failed:
        RTS

play_frame:
        LEA     player_state(PC),A3
        TST.B   STATE_FINISHED(A3)
        BNE.S   play_frame_done
        MOVEA.L STATE_STREAM(A3),A0
        CMPA.L  STATE_END(A3),A0
        BCS.S   play_frame_record
        MOVEA.L STATE_LOOP(A3),A0
        MOVE.L  A0,D0
        BNE.S   play_frame_record
        BSR.W   qsound_silence
        MOVE.B  #1,STATE_FINISHED(A3)
        BRA.S   play_frame_done
play_frame_record:
        MOVEQ   #0,D7
        MOVE.B  (A0)+,D7
        LSL.W   #8,D7
        MOVE.B  (A0)+,D7
        TST.W   D7
        BEQ.S   play_frame_store
        SUBQ.W  #1,D7
        MOVEA.L #QSOUND2_ADDRESS,A2
play_frame_writes:
        MOVE.B  (A0)+,(A2)
        MOVE.B  (A0)+,2(A2)
        DBF     D7,play_frame_writes
play_frame_store:
        MOVE.L  A0,STATE_STREAM(A3)
play_frame_done:
        RTS

qsound_reset:
        MOVEQ   #0,D6
qsound_reset_loop:
        MOVEQ   #0,D1
        CMPI.B  #7,D6
        BNE.S   qsound_reset_value
        MOVEQ   #$3F,D1
qsound_reset_value:
        MOVE.W  D6,D0
        BSR.S   qsound_write
        ADDQ.W  #1,D6
        CMPI.W  #14,D6
        BNE.S   qsound_reset_loop
        MOVEQ   #0,D1
        MOVEQ   #$27,D0
        BSR.S   qsound_write
        MOVEQ   #0,D1
        MOVEQ   #$28,D0
        BSR.S   qsound_write
        MOVEQ   #1,D1
        MOVEQ   #$28,D0
        BSR.S   qsound_write
        MOVEQ   #2,D1
        MOVEQ   #$28,D0
        BRA.W   qsound_write

qsound_silence:
        MOVEQ   #$3F,D1
        MOVEQ   #7,D0
        BSR.S   qsound_write
        MOVEQ   #0,D1
        MOVEQ   #8,D0
        BSR.S   qsound_write
        MOVEQ   #9,D0
        BSR.S   qsound_write
        MOVEQ   #10,D0
        BSR.S   qsound_write
        MOVEQ   #0,D1
        MOVEQ   #$28,D0
        BSR.S   qsound_write
        MOVEQ   #1,D1
        MOVEQ   #$28,D0
        BSR.S   qsound_write
        MOVEQ   #2,D1
        MOVEQ   #$28,D0
        BRA.W   qsound_write

qsound_write:
        MOVEA.L #QSOUND2_ADDRESS,A2
        MOVE.B  D0,(A2)
        MOVE.B  D1,2(A2)
        RTS

* Decode one bounds-checked QLZ1 stream. A0/A1 are input/output, A2/A3 are
* their allocation ends. D0 returns zero on success and -1 on malformed data.
qlz_decompress:
        MOVEM.L D4-D7/A4-A5,-(A7)
        MOVE.L  A2,D0
        SUB.L   A0,D0
        CMPI.L  #QLZ_HEADER_SIZE,D0
        BCS.W   qlz_failed
        CMPI.L  #QLZ_MAGIC,(A0)+
        BNE.W   qlz_failed
        MOVE.L  (A0)+,D4
        MOVE.L  (A0)+,D5
        MOVE.L  A0,D0
        ADD.L   D5,D0
        CMP.L   A2,D0
        BNE.W   qlz_failed
        MOVE.L  A3,D0
        SUB.L   A1,D0
        CMP.L   D4,D0
        BNE.W   qlz_failed
        MOVEA.L A1,A4
        MOVEQ   #0,D7
qlz_next:
        CMPA.L  A3,A1
        BEQ.W   qlz_complete
        CMPA.L  A2,A0
        BCC.W   qlz_failed
        MOVEQ   #0,D0
        MOVE.B  (A0)+,D0
        MOVE.L  D0,D1
        LSR.B   #6,D1
        BNE.S   qlz_match

        ANDI.L  #$3F,D0
        ADDQ.L  #1,D0
        MOVE.L  A2,D1
        SUB.L   A0,D1
        CMP.L   D0,D1
        BCS.W   qlz_failed
        MOVE.L  A3,D1
        SUB.L   A1,D1
        CMP.L   D0,D1
        BCS.W   qlz_failed
        MOVE.W  D0,D1
        SUBQ.W  #1,D1
qlz_literal_loop:
        MOVE.B  (A0)+,(A1)+
        DBF     D1,qlz_literal_loop
        MOVEQ   #0,D7
        BRA.W   qlz_next

qlz_match:
        ANDI.L  #$3F,D0
        ADDQ.L  #3,D0
        CMPI.B  #1,D1
        BNE.S   qlz_not_short
        CMPA.L  A2,A0
        BCC.W   qlz_failed
        MOVEQ   #0,D7
        MOVE.B  (A0)+,D7
        ADDQ.L  #1,D7
        BRA.S   qlz_copy_match
qlz_not_short:
        CMPI.B  #2,D1
        BNE.S   qlz_continuation
        MOVE.L  A2,D2
        SUB.L   A0,D2
        CMPI.L  #2,D2
        BCS.W   qlz_failed
        MOVEQ   #0,D7
        MOVE.B  (A0)+,D7
        LSL.W   #8,D7
        MOVE.B  (A0)+,D7
        TST.L   D7
        BEQ.W   qlz_failed
        BRA.S   qlz_copy_match
qlz_continuation:
        TST.L   D7
        BEQ.W   qlz_failed
qlz_copy_match:
        MOVE.L  A1,D2
        SUB.L   A4,D2
        CMP.L   D7,D2
        BCS.W   qlz_failed
        MOVE.L  A3,D2
        SUB.L   A1,D2
        CMP.L   D0,D2
        BCS.W   qlz_failed
        MOVEA.L A1,A5
        SUBA.L  D7,A5
        MOVE.W  D0,D1
        SUBQ.W  #1,D1
qlz_match_loop:
        MOVE.B  (A5)+,(A1)+
        DBF     D1,qlz_match_loop
        BRA.W   qlz_next

qlz_complete:
        CMPA.L  A2,A0
        BNE.S   qlz_failed
        MOVEQ   #0,D0
        BRA.S   qlz_return
qlz_failed:
        MOVEQ   #-1,D0
qlz_return:
        MOVEM.L (A7)+,D4-D7/A4-A5
        RTS

player_state:
        DS.B    14
        EVEN
