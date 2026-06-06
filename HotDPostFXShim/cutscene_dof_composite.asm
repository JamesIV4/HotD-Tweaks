ps_3_0
def c0, 1.5, 0.5, -0.5, 1
def c9, 0.212500006, 0.715399981, 0.0720999986, 21.5324554
def c13, 2, 0, 0, 0
dcl_texcoord v0.xy
dcl_2d s0
dcl_2d s1
dcl_2d s3
dcl_2d s4
dcl_2d s5
dcl_2d s6
texld_pp r0, v0, s1
add r4.xy, v0, c14
texld_pp r1, r4, s5
add r4.xy, v0, c15
texld_pp r2, r4, s0
mul_sat r4.z, r2.w, c14.z
lrp_pp r3.xyz, r4.z, r1, r0
add r0.xy, -r2.wxzw, c0
mov_sat r0.x, r0.x
texld_pp r1, v0, s3
mul r0.z, r1.w, c1.y
mul_pp r0.x, r0.x, r0.z
mad_pp r0.xyz, r0.y, -r0.x, r3
mad_pp r0.xyz, c2.x, r1, r0
dp3_pp r0.w, r0, c9
lrp_pp r1.xyz, c8.x, r0, r0.w
mul_pp r0.xyz, r1, c4
mad_pp r1.xyz, r0.w, c3, -r0
mov_pp r2.x, c5.x
mad_sat_pp r1.w, r2.x, r0.w, c6.x
mad_pp r0.xyz, r1.w, r1, r0
add_pp r0.xyz, r0, c0.z
mov r1.yw, c0
mad_pp r0.xyz, r0, c7.x, r1.y
add_pp r1.xyz, -r0, c0.w
cmp_pp r0.xyz, -c10.x, r0, r1
add r1.xy, c11.w, v0
mul_pp r1.xy, r1, c9.w
texld r2, r1, s4
add_pp r1.xyz, r2, c0.z
rcp r2.x, c11.z
mul_sat_pp r0.w, r0.w, r2.x
lrp_pp r2.x, r0.w, c11.x, c11.y
mad_sat_pp r0.xyz, r1, r2.x, r0
texld r2, v0, s6
add_pp r0.w, -r2.x, c13.x
mad_pp r0.w, r0.w, c12.x, r1.w
mul_pp oC0.xyz, r0, r0.w
mov_pp oC0.w, c0.w
