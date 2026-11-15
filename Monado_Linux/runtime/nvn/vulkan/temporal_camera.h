// [XCDE] LINUX-103: world-space temporal camera continuity.
// Design reference: Monado-Reborn T260 (MIT), commit3cc9eeb4. A view's
// translation is -R*position, so comparing that column confuses small turns
// far from the origin with teleports. This implementation solves R*p=-t,
// including finite non-unit bases, rather than assuming an exact transpose.
#pragma once
#include <stdbool.h>
#include <math.h>

static inline bool tmp_camera_position(const float view[12], double position[3]) {
    double a[3][4];
    for (int r=0;r<3;r++) for(int k=0;k<4;k++) {
        if (!isfinite(view[r*4+k])) return false;
        a[r][k] = k==3 ? -(double)view[r*4+k] : (double)view[r*4+k];
    }
    for(int c=0;c<3;c++) {
        int pivot=c;
        for(int r=c+1;r<3;r++) if(fabs(a[r][c])>fabs(a[pivot][c])) pivot=r;
        if(fabs(a[pivot][c])<1e-10) return false;
        if(pivot!=c) for(int k=c;k<4;k++){double t=a[c][k];a[c][k]=a[pivot][k];a[pivot][k]=t;}
        double scale=a[c][c];for(int k=c;k<4;k++)a[c][k]/=scale;
        for(int r=0;r<3;r++)if(r!=c){double f=a[r][c];for(int k=c;k<4;k++)a[r][k]-=f*a[c][k];}
    }
    for(int k=0;k<3;k++){position[k]=a[k][3];if(!isfinite(position[k]))return false;}
    return true;
}

static inline bool tmp_camera_continuous(const float current[12], const float previous[12]) {
    double c[3],p[3],rotation=0;
    if(!tmp_camera_position(current,c)||!tmp_camera_position(previous,p))return false;
    for(int r=0;r<3;r++)for(int k=0;k<3;k++)rotation+=fabs((double)current[r*4+k]-previous[r*4+k]);
    double distance2=0;for(int k=0;k<3;k++){double d=c[k]-p[k];distance2+=d*d;}
    return isfinite(rotation)&&isfinite(distance2)&&rotation<=1.5&&distance2<=64.0;
}
