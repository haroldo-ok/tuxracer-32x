!-----------------------------------------------------------------------
! Tux Racer 32X - flat triangle rasteriser (SH-2 assembly).
!
!   void raster_flush_asm(const stri_t *tris, const u16 *order, int n,
!                         u8 *fb)
!
! Draws tris[order[n-1]] .. tris[order[0]] (far to near) into the 8bpp
! 320x224 framebuffer at fb.  Pixel coverage is identical to fill_tri_s()
! + raster_tri_c() in src/core/render.c (the bench ROM cross-checks the
! two on random triangles): vertices sorted by y, edges stepped in 16.16
! from the scanline centre, span [xl >> 16, xr >> 16) clipped to 0..319.
!
! stri_t (16 bytes): s16 x0,y0,x1,y1,x2,y2; u8 color, flags; u16 key.
! Coordinates are within +-2047 and triangles taller than 1023 rows are
! skipped, so every edge slope comes from recip30_tab (no divides).
!
! Each scanline is written as whole 32-bit words through the overwrite
! image (+0x20000): bytes that are zero in the written value are not
! stored, so the partial first/last words are masked instead of needing
! read-modify-write or byte stores.  Colour 0 therefore draws nothing
! (triangle colours are palette ramps >= 16).
!
! Frame (r15 relative), all 32-bit:
!    0 D02   4 X02   8 D01  12 X01  16 D12  20 X12  24 MIDLEFT  28 ROWS2
!   32 CC32 36 (unused) 40 FB(ovr)  44 TRIS  48 ORDER  52 N
!-----------------------------------------------------------------------

        .set    D02, 0
        .set    X02, 4
        .set    D01, 8
        .set    X01, 12
        .set    D12, 16
        .set    X12, 20
        .set    MIDLEFT, 24
        .set    ROWS2, 28
        .set    CC32, 32
        .set    FBO, 40
        .set    TRIS, 44
        .set    ORDER, 48
        .set    NCNT, 52
        .set    FRAME, 56

        .text
        .align  2
        .global _raster_flush_asm
_raster_flush_asm:
        tst     r6, r6
        bt      .Lret0
        mov.l   r8, @-r15
        mov.l   r9, @-r15
        mov.l   r10, @-r15
        mov.l   r11, @-r15
        mov.l   r12, @-r15
        mov.l   r13, @-r15
        mov.l   r14, @-r15
        add     #-FRAME, r15
        mov.l   r4, @(TRIS, r15)
        mov.l   r6, @(NCNT, r15)
        mov     r6, r0
        add     r0, r0
        add     #-2, r0
        add     r0, r5                  ! &order[n-1]
        mov.l   r5, @(ORDER, r15)
        mov.l   .Lovr, r0
        add     r0, r7
        mov.l   r7, @(FBO, r15)         ! framebuffer, overwrite image
        mov.l   .Lmasks, r4             ! r4 = mask table (constant)
        mov.w   .Lc320, r12             ! r12 = 320 (constant)
        mov.l   .Lrecip, r14            ! r14 = recip30_tab (constant)

!------------------------------------------------------------ per triangle
.Ltri:
        mov.l   @(ORDER, r15), r11
        mov.w   @r11, r0                ! index (< 1500)
        add     #-2, r11
        mov.l   r11, @(ORDER, r15)
        shll2   r0
        shll2   r0
        mov.l   @(TRIS, r15), r10
        add     r0, r10                 ! &tris[index]
        mov.w   @r10+, r1               ! x0
        mov.w   @r10+, r2               ! y0
        mov.w   @r10+, r3               ! x1
        mov.w   @r10+, r5               ! y1
        mov.w   @r10+, r6               ! x2
        mov.w   @r10+, r7               ! y2
        mov.b   @r10, r0                ! colour
        extu.b  r0, r0
        mov     r0, r10
        shll8   r10
        or      r10, r0
        mov     r0, r10
        shll16  r10
        or      r10, r0
        mov.l   r0, @(CC32, r15)

        ! sort the vertices by y (same swap sequence as the C code)
        cmp/ge  r2, r5                  ! y1 >= y0 ?
        bt      1f
        mov     r1, r0
        mov     r3, r1
        mov     r0, r3
        mov     r2, r0
        mov     r5, r2
        mov     r0, r5
1:      cmp/ge  r2, r7                  ! y2 >= y0 ?
        bt      2f
        mov     r1, r0
        mov     r6, r1
        mov     r0, r6
        mov     r2, r0
        mov     r7, r2
        mov     r0, r7
2:      cmp/ge  r5, r7                  ! y2 >= y1 ?
        bt      3f
        mov     r3, r0
        mov     r6, r3
        mov     r0, r6
        mov     r5, r0
        mov     r7, r5
        mov     r0, r7
3:
        cmp/eq  r2, r7                  ! zero height
        bt      .Ltri_next
        cmp/pl  r7                      ! entirely above the screen
        bf      .Ltri_next
        mov.w   .Lc224, r0
        cmp/ge  r0, r2                  ! entirely below
        bt      .Ltri_next
        mov     r7, r10
        sub     r2, r10
        mov.w   .Lc1023, r0
        cmp/gt  r0, r10                 ! taller than the slope table
        bt      .Ltri_next

        ! ya = max(y0, 0) -> r8 ; yc = min(y2, 224) -> r9 ; yb = clamp(y1) -> r13
        mov     r2, r8
        cmp/pz  r8
        bt      4f
        mov     #0, r8
4:      mov     r7, r9
        mov.w   .Lc224, r0
        cmp/gt  r0, r9
        bf      5f
        mov     r0, r9
5:      mov     r5, r13
        cmp/ge  r8, r13
        bt      6f
        mov     r8, r13
6:      cmp/gt  r9, r13
        bf      7f
        mov     r9, r13
7:
        ! d02 = ((x2 - x0) << 16) * recip30[y2 - y0] >> 30
        mov     r10, r0                 ! dy02
        shll2   r0
        mov.l   @(r0, r14), r10
        mov     r6, r0
        sub     r1, r0
        shll16  r0
        dmuls.l r0, r10
        sts     macl, r0
        sts     mach, r10
        shll2   r10
        rotl    r0
        rotl    r0
        and     #3, r0
        or      r10, r0
        mov.l   r0, @(D02, r15)
        ! x02 = (x0 << 16) + 0x8000 + fxmul(d02, ((ya - y0) << 16) + 0x8000)
        mov     r0, r10
        mov     r8, r0
        sub     r2, r0
        add     r0, r0
        add     #1, r0
        shll16  r0
        shlr    r0
        dmuls.l r0, r10
        sts     mach, r10
        sts     macl, r0
        xtrct   r10, r0
        mov     r1, r10
        shll16  r10
        add     r10, r0
        mov.l   .Lhalf, r10
        add     r10, r0
        mov.l   r0, @(X02, r15)

        ! edge 0-1 (only when y1 > y0; otherwise the upper half is empty)
        cmp/gt  r2, r5
        bf      .Lflat_top
        mov     r5, r0
        sub     r2, r0                  ! dy01
        shll2   r0
        mov.l   @(r0, r14), r10
        mov     r3, r0
        sub     r1, r0
        shll16  r0
        dmuls.l r0, r10
        sts     macl, r0
        sts     mach, r10
        shll2   r10
        rotl    r0
        rotl    r0
        and     #3, r0
        or      r10, r0                 ! d01
        mov.l   r0, @(D01, r15)
        mov.l   @(D02, r15), r10
        cmp/gt  r0, r10                 ! d01 < d02 -> middle vertex on the left
        movt    r10
        mov.l   r10, @(MIDLEFT, r15)
        mov     r0, r10
        mov     r8, r0
        sub     r2, r0
        add     r0, r0
        add     #1, r0
        shll16  r0
        shlr    r0
        dmuls.l r0, r10
        sts     mach, r10
        sts     macl, r0
        xtrct   r10, r0
        mov     r1, r10
        shll16  r10
        add     r10, r0
        mov.l   .Lhalf, r10
        add     r10, r0                 ! x01
        bra     .Ledge12
        mov.l   r0, @(X01, r15)
.Lflat_top:
        cmp/gt  r3, r1                  ! x1 < x0 -> middle vertex on the left
        movt    r0
        mov.l   r0, @(MIDLEFT, r15)

.Ledge12:
        ! edge 1-2 (only when y2 > y1; otherwise the lower half is empty)
        cmp/gt  r5, r7
        bf      .Lsetup
        mov     r7, r0
        sub     r5, r0                  ! dy12
        shll2   r0
        mov.l   @(r0, r14), r10
        mov     r6, r0
        sub     r3, r0
        shll16  r0
        dmuls.l r0, r10
        sts     macl, r0
        sts     mach, r10
        shll2   r10
        rotl    r0
        rotl    r0
        and     #3, r0
        or      r10, r0                 ! d12
        mov.l   r0, @(D12, r15)
        mov     r0, r10
        mov     r13, r0
        sub     r5, r0                  ! yb - y1
        add     r0, r0
        add     #1, r0
        shll16  r0
        shlr    r0
        dmuls.l r0, r10
        sts     mach, r10
        sts     macl, r0
        xtrct   r10, r0
        mov     r3, r10
        shll16  r10
        add     r10, r0
        mov.l   .Lhalf, r10
        add     r10, r0                 ! x12
        mov.l   r0, @(X12, r15)

.Lsetup:
        mov     r13, r5
        sub     r8, r5                  ! rows1 = yb - ya
        mov     r9, r0
        sub     r13, r0                 ! rows2 = yc - yb
        mov.l   r0, @(ROWS2, r15)
        mov.l   @(FBO, r15), r10
        mul.l   r8, r12
        sts     macl, r0
        add     r0, r10                 ! r10 = first scanline
        mov.l   @(CC32, r15), r11
        mov.l   @(MIDLEFT, r15), r0
        tst     r5, r5
        bf      .Lnormal
        ! no upper half: start with edge 1-2 on the middle side
        mov.l   @(ROWS2, r15), r5
        mov     #0, r1
        mov.l   r1, @(ROWS2, r15)
        tst     r0, r0
        bt      .Lft_right
        mov.l   @(X12, r15), r6
        mov.l   @(D12, r15), r7
        mov.l   @(X02, r15), r8
        bra     .Lgo
        mov.l   @(D02, r15), r9
.Lft_right:
        mov.l   @(X02, r15), r6
        mov.l   @(D02, r15), r7
        mov.l   @(X12, r15), r8
        bra     .Lgo
        mov.l   @(D12, r15), r9
.Lnormal:
        tst     r0, r0
        bt      .Ln_right
        mov.l   @(X01, r15), r6
        mov.l   @(D01, r15), r7
        mov.l   @(X02, r15), r8
        bra     .Lgo
        mov.l   @(D02, r15), r9
.Ln_right:
        mov.l   @(X02, r15), r6
        mov.l   @(D02, r15), r7
        mov.l   @(X01, r15), r8
        mov.l   @(D01, r15), r9
.Lgo:
        cmp/pl  r5
        bt      .Lrow
        bra     .Ltri_next
        nop

        .align  2
.Lovr:  .long   0x00020000
.Lmasks: .long  masks
.Lrecip: .long  _recip30_tab
.Lhalf: .long   0x00008000
.Lc320: .short  320
.Lc224: .short  224
.Lc1023: .short 1023

!------------------------------------------------------------- scanlines
! r4 masks, r5 rows, r6 xl, r7 dl, r8 xr, r9 dr, r10 row, r11 colour x4,
! r12 320; r0-r3, r13 temporaries
        .align  2
.Lrow:
        swap.w  r6, r1
        exts.w  r1, r1                  ! l = xl >> 16
        swap.w  r8, r2
        exts.w  r2, r2                  ! r = xr >> 16
        cmp/pz  r1
        bf      .Lfixl
.Lretl:
        cmp/gt  r12, r2
        bt      .Lfixr
.Lretr:
        cmp/ge  r2, r1                  ! empty span ?
        bt      .Lnext
        mov     r1, r0
        and     #3, r0
        shll2   r0
        mov.l   @(r0, r4), r3
        and     r11, r3                 ! first word, left bytes masked
        mov     r2, r0
        and     #3, r0
        shll2   r0
        add     #16, r0
        mov.l   @(r0, r4), r13
        and     r11, r13                ! last word, right bytes masked
        add     #-1, r2
        mov     #-4, r0
        and     r0, r1
        and     r0, r2
        add     r10, r1                 ! p0 = &row[l & ~3]
        add     r10, r2                 ! p1 = &row[(r - 1) & ~3]
        cmp/eq  r1, r2
        bf      .Lmulti
        and     r13, r3                 ! single word: both masks
        bra     .Lnext
        mov.l   r3, @r1
.Lmulti:
        mov.l   r3, @r1
        mov.l   r13, @r2
        add     #4, r1
        cmp/hs  r2, r1
        bt      .Lnext
.Lmid:
        mov.l   r11, @r1
        add     #4, r1
        cmp/hs  r2, r1
        bf      .Lmid
.Lnext:
        add     r7, r6
        add     r9, r8
        dt      r5
        bf/s    .Lrow
        add     r12, r10

        ! second half: swap in edge 1-2
        mov.l   @(ROWS2, r15), r5
        cmp/pl  r5
        bf      .Ltri_next
        mov     #0, r0
        mov.l   r0, @(ROWS2, r15)
        mov.l   @(MIDLEFT, r15), r0
        tst     r0, r0
        bt      .Lsw_right
        mov.l   @(X12, r15), r6
        bra     .Lrow
        mov.l   @(D12, r15), r7
.Lsw_right:
        mov.l   @(X12, r15), r8
        bra     .Lrow
        mov.l   @(D12, r15), r9

.Lfixl:
        bra     .Lretl
        mov     #0, r1
.Lfixr:
        bra     .Lretr
        mov     r12, r2

.Ltri_next:
        mov.l   @(NCNT, r15), r0
        dt      r0
        mov.l   r0, @(NCNT, r15)
        bf      .Ltri

        add     #FRAME, r15
        mov.l   @r15+, r14
        mov.l   @r15+, r13
        mov.l   @r15+, r12
        mov.l   @r15+, r11
        mov.l   @r15+, r10
        mov.l   @r15+, r9
        rts
        mov.l   @r15+, r8
.Lret0:
        rts
        nop

        .align  2
masks:  .long   0xffffffff, 0x00ffffff, 0x0000ffff, 0x000000ff   ! left
        .long   0xffffffff, 0xff000000, 0xffff0000, 0xffffff00   ! right
