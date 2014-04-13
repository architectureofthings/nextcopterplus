//***********************************************************
//* imu.c
//*
//* IMU code ported from KK2V1_1V12S1Beginner code
//* by Rolf Bakke and Steveis
//*
//* Ported to OpenAeroVTOL by David Thompson (C)2014
//*
//***********************************************************

//***********************************************************
//* Includes
//***********************************************************

#include "compiledefs.h"
#include <avr/io.h>
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>
#include "io_cfg.h"
#include "acc.h"
#include "gyros.h"
#include "main.h"
#include <avr/pgmspace.h> 
#include "menu_ext.h"
#include "rc.h"

//************************************************************
// IMU Prototypes
//************************************************************

void simple_imu_update(uint32_t period);
void Rotate3dVector(void);
void ExtractEulerAngles(void);

float small_sine(float angle);
float small_cos(float angle);
void thetascale(float gyro, float interval);
void RotateVector(float angle);
float ext2(float Vector);
void reset_IMU(void);

//************************************************************
// 	Defines
//************************************************************

#define ACCSENSITIVITY		128.0f		// Calculate factor for Acc to report directly in g
										// For +/-4g FS, accelerometer sensitivity (+/-512 / +/-4g) = 1024/8 = 128

#ifdef KK21
#define GYROSENSRADIANS		0.06818f	// Calculate factor for Gyros to report directly in rad/s
										// For +/-2000 deg/s FS, gyro sensitivity for 10 bits (+/-512) = (4000/1024) = 3.90625 deg/s/lsb
										// 3.90625 * Pi/180 = 0.06818f rad/s/lsb
#else
#define GYROSENSRADIANS		0.017045f	// KK2.0 has 500deg/s gyros compared with the KK2.1's 2000deg/s
#endif
										
#define SMALLANGLEFACTOR	0.66f		// Empirically calculated to produce exactly 20 at 20 degrees. Was 0.66			
										
										// Acc magnitude values - based on MultiWii 2.3 values
#define acc_1_15G_SQ 21668				// (1.15 * ACCSENSITIVITY) * (1.15 * ACCSENSITIVITY)
#define acc_0_85G_SQ 11837				// (0.85 * ACCSENSITIVITY) * (0.85 * ACCSENSITIVITY)												

//************************************************************
// 	Globals
//************************************************************

float VectorA, VectorB;

float VectorX = 0;						// Initialise the vector to point straight up
float VectorY = 0;
float VectorZ = 1;

float VectorNewA, VectorNewB;
float LengthVector, theta;
float GyroPitchVC, GyroRollVC;
float AccAnglePitch, AccAngleRoll, EulerAngleRoll, EulerAnglePitch;

float 	accSmooth[NUMBEROFAXIS];		// Filtered acc data
int16_t	angle[2]; 						// Attitude in degrees
float	interval;						// Interval in seconds since the last loop
	
//************************************************************
// Code
//
// Assume that all gyro (gyroADC[]) and acc signals (accADC[]) are 
// calibrated and have the appropriate zeros removed. At rest, only the 
// Z acc has a value, usually around +128, for the 1g earth gravity vector
//
// Notes:
// 1. KK code loads the MPU6050's ACCEL_XOUT data into AccY and ACCEL_YOUT into AccX (swapping X/Y axis)
// 2. In KK code GyroPitch gets GYRO_XOUT, GyroRoll gets GYRO_YOUT. GyroYaw gets GYRO_ZOUT.
//	  This means that AccX(ACCEL_YOUT) and GyroPitch are the pitch components, and AccY(ACCEL_XOUT) and GyroRoll are the roll components 
//	  In other words, AccX(actually ACCEL_YOUT) and GYRO_XOUT are cludged to work together, which looks right, but by convention is wrong.
//	  By IMU convention, rotation around a gyro's X axis, for example, will cause the acc's Y axis to change.
// 3. Of course, OpenAero code does exactly the same cludge, lol. But in this case, pitch and roll *gyro* data are swapped. 
//	  The result is the same, pairing opposing (X/Y/Z) axis together to the same (R/P/Y) axis name.
//
//		Actual hardware	  KK2 code		  OpenAero2 code
//		ACCEL_XOUT		= AccY*			=	accADC[ROLL]
//		ACCEL_YOUT		= AccX*			=	accADC[PITCH]
//		ACCEL_ZOUT		= AccZ			=	accADC[YAW/Z]
//		GYRO_XOUT		= GyroPitch		=	gyroADC[PITCH]*
//		GYRO_YOUT		= GyroRoll		=	gyroADC[ROLL]*
//		GYRO_ZOUT		= GyroYaw		=	gyroADC[YAW]
//
//		* = swapped axis
//
//************************************************************

void simple_imu_update(uint32_t period)
{
	float		tempf;
	int8_t		axis;
	uint32_t	roll_sq, pitch_sq, yaw_sq;
	uint32_t 	AccMag = 0;
		
	// Work out interval in seconds
	// Convert (period) from units of 400ns (1/2500000) to seconds (1s/400ns = 2500000)
	tempf = period; // Promote int16_t to float
	interval = tempf/2500000.0f;		
	
	// Smooth Acc signals - note that accSmooth is in [ROLL, PITCH, YAW] order
	for (axis = 0; axis < NUMBEROFAXIS; axis++)
	{
		// Acc LPF
		if (Config.Acc_LPF > 1)
		{
			// Acc LPF
			accSmooth[axis] = ((accSmooth[axis] * (float)(Config.Acc_LPF - 1)) - (float)(accADC[axis])) / Config.Acc_LPF;
		}
		else
		{
			// Use raw accADC[axis] as source for acc values
			accSmooth[axis] =  -accADC[axis];
		}
	}
	
	// Add correction data to gyro inputs based on difference between Euler angles and acc angles
	AccAngleRoll = accSmooth[ROLL] * SMALLANGLEFACTOR;		// KK2 - AccYfilter
	AccAnglePitch = accSmooth[PITCH] * SMALLANGLEFACTOR;

	// Copy/promote gyro values for rotate
	GyroRollVC = gyroADC[ROLL];								// KK2 - GyroRoll
	GyroPitchVC = gyroADC[PITCH];

	// Calculate acceleration magnitude.
	// This will determine the method used for estimating attitude
	roll_sq = (accADC[ROLL] * accADC[ROLL]);
	pitch_sq = (accADC[PITCH] * accADC[PITCH]) ;
	yaw_sq = (accADC[YAW] * accADC[YAW]);
	AccMag = (uint16_t)(roll_sq + pitch_sq + yaw_sq);

	// Add acc correction if inside local acceleration bounds and not inverted according to VectorZ 
	// This is actually a kind of Complementary Filter
	if	((AccMag > acc_0_85G_SQ) && (AccMag < acc_1_15G_SQ) && (VectorZ > 0.5))
	{
		tempf = (EulerAngleRoll - AccAngleRoll) / Config.CF_factor; // Default Config.CF_factor is 4
		GyroRollVC = GyroRollVC + tempf;
		
		tempf = (EulerAnglePitch - AccAnglePitch) / Config.CF_factor;
		GyroPitchVC = GyroPitchVC + tempf;
	}
	
	// Rotate up-direction 3D vector with gyro inputs
	Rotate3dVector();
	ExtractEulerAngles();
	
	// Upscale to 0.01 degrees resolution and copy to angle[] for display
	angle[ROLL] = (int16_t)(EulerAngleRoll * -100);
	angle[PITCH] = (int16_t)(EulerAnglePitch * -100);
}

void Rotate3dVector(void)
{
	// Rotate around X axis (pitch)
	thetascale(GyroPitchVC, interval);
	VectorA = VectorY;
	VectorB = VectorZ;
	RotateVector(theta);
	VectorY = VectorNewA;
	VectorZ = VectorNewB;

	// Rotate around Y axis (roll)
	thetascale (GyroRollVC, interval);
	VectorA = VectorX;
	VectorB = VectorZ;
	RotateVector(theta);
	VectorX = VectorNewA;
	VectorZ = VectorNewB;

	// Rotate around Z axis (yaw)
	thetascale(gyroADC[YAW], interval);
	VectorA = VectorX;
	VectorB = VectorY;
	RotateVector(theta);
	VectorX = VectorNewA;
	VectorY = VectorNewB;
}

void RotateVector(float angle)
{
	VectorNewA = VectorA * small_cos(angle) - VectorB * small_sine(angle);
	VectorNewB = VectorA * small_sine(angle) + VectorB * small_cos(angle);
	
	// Keep Vectors within manageable bounds. Can lock up otherwise.
	// This seem to only affect operation via the sensor display screen.
/*	if (VectorNewA < -2) VectorNewA = -2;
	if (VectorNewA > 2)	VectorNewA = 2;
	if (VectorNewB < -2) VectorNewB = -2;
	if (VectorNewB > 2) VectorNewB = 2;*/
}

void thetascale(float gyro, float interval)
{
	// interval = time in seconds since last measurement
	// GYROSENSRADIANS = conversion from raw gyro data to rad/s (0.06818f for my code)
	// theta = actual number of radians moved

	theta = (gyro * GYROSENSRADIANS * interval);
}

// Small angle approximations of Sine, Cosine
float small_sine(float angle)
{
	//sin(angle) = angle
	return angle;
}

float small_cos(float angle)
{
	// cos(angle) = (1 - (angle^2 / 2))
	float temp;
	
	temp = (angle * angle) / 2;
	temp = 1 - temp;
	return temp;
}

void ExtractEulerAngles(void)
{
	EulerAngleRoll = ext2(VectorX);
	EulerAnglePitch = ext2(VectorY);
}

float ext2(float Vector)
{
	float temp;
	
	// Rough translation to Euler
	temp = Vector * 90;

	// Change 0-90-0 to 0-90-180 so that 
	// swap happens at 100% inverted
	if (VectorZ < 0)
	{
		// CW rotations
		if (temp > 0)
		{
			temp = 180 - temp;
		}
		// CCW rotations
		else
		{
			temp = -180 - temp;
		}
	}

	return (temp);
}

void reset_IMU(void)
{
	VectorX = 0;						// Initialise the vector to point straight up
	VectorY = 0;
	VectorZ = 1;
}
