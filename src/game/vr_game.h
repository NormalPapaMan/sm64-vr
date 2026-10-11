#ifndef VR_GAME_H
#define VR_GAME_H

#include <PR/ultratypes.h>
#include <PR/os_cont.h>
#include "types.h"

#include "pc/vr/vr.h"

struct GraphNodeCamera;
struct GraphNodeRotation;
struct GraphNode;
struct Object;

// main loop
void vr_game_begin_tick(void);
void vr_game_commit(void);
void vr_game_frame(double dt);

// camera
bool vr_fp_active(void);
s16 vr_fp_camera_yaw(void);
void vr_camera_override(struct GraphNodeCamera *gc);
void vr_capture_camera(Mat4 view);

// Mario rendering hooks (rendering_graph_node.c / mario_misc.c)
void vr_object_begin(struct Object *obj);
void vr_object_end(struct Object *obj);
void vr_mario_head(struct GraphNode *node, Mat4 mtx);
bool vr_hide_rotation_node(struct GraphNodeRotation *node);
void vr_anim_part(Mat4 mtx);
s32 vr_hand_case(s32 rightHand, s32 defaultCase);

// controller backend
void vr_game_read_pad(OSContPad *pad);

#endif
