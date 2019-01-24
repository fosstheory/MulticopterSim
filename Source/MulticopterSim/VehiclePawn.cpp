/*
* VehiclePawn.cpp: Class implementation for pawn class in MulticopterSim
*
* Copyright (C) 2018 Simon D. Levy
*
* MIT License
*/

#include "VehiclePawn.h"
#include "UObject/ConstructorHelpers.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Runtime/Core/Public/Math/UnrealMathUtility.h"

#include <shlwapi.h>
#include "joystickapi.h"

#include <cmath>

// Main firmware
hf::Hackflight hackflight;

// MSP comms
#include "msppg/MSPPG.h"

// PID controllers
#include <pidcontrollers/level.hpp>
#include <pidcontrollers/althold.hpp>
#include <pidcontrollers/poshold.hpp>

// Python support
#include "python/PythonLoiter.h"

// Socket comms
//static const char * HOST = "137.113.118.68";  // thales.cs.wlu.edu
//static const char * HOST = "129.97.45.86";      // uwaterloo ctn
static const char * HOST = "127.0.0.1";         // localhost
static const int PORT = 20000;
ThreadedSocketServer server = ThreadedSocketServer(PORT, HOST);

// Debugging
static const FColor TEXT_COLOR = FColor::Yellow;
static const float  TEXT_SCALE = 2.f;

// Scaling constant for turning motor spin to thrust
static const float THRUST_FACTOR = 130;

void hf::Board::outbuf(char * buf)
{
	// on screen
	if (GEngine) {

		// -1 = no overwrite (0 for overwrite); 5.f = arbitrary time to display; true = newer on top
		GEngine->AddOnScreenDebugMessage(0, 5.f, TEXT_COLOR, FString(buf), true, FVector2D(TEXT_SCALE,TEXT_SCALE));
	}

	// on Visual Studio output console
	OutputDebugStringA(buf);
}

// PID tuning

hf::Rate ratePid = hf::Rate(
	0.01,	// Roll/Pitch P
	0.01,	// Roll/Pitch I
	0.01,	// Roll/Pitch D
	0.5,	// Yaw P
	0.0,	// Yaw I
	8.f);	// Demands to rate


hf::Level level = hf::Level(0.20f);

#ifdef _PYTHON
PythonLoiter loiter = PythonLoiter(
	0.5f,	// Altitude P
	1.0f,	// Altitude D
	0.2f);	// Cyclic P
#else

hf::AltitudeHold althold = hf::AltitudeHold(
	1.00f,  // altHoldP
	0.50f,  // altHoldVelP
	0.01f,  // altHoldVelI
	0.10f); // altHoldVelD

hf::PositionHold poshold = hf::PositionHold(
	0.2,	// posP
	0.2f,	// posrP
	0.0f);	// posrI

#endif

// Mixer
#include <mixers/quadx.hpp>
MixerQuadX mixer;


// APawn methods ---------------------------------------------------

AVehiclePawn::AVehiclePawn()
{
	// Structure to hold one-time initialization
	struct FConstructorStatics
	{
		ConstructorHelpers::FObjectFinderOptional<UStaticMesh> PlaneMesh;
		FConstructorStatics()
			: PlaneMesh(TEXT("/Game/Flying/Meshes/3DFly.3DFly"))
		{
		}
	};
	static FConstructorStatics ConstructorStatics;

	// Create static mesh component
	PlaneMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlaneMesh0"));
	PlaneMesh->SetStaticMesh(ConstructorStatics.PlaneMesh.Get());	// Set static mesh
	RootComponent = PlaneMesh;

    // Create controller
    flightController = SimFlightController::createSimFlightController();

    // Create receiver (joystick)
    joystickInit();

	// Start Hackflight firmware, indicating already armed
	hackflight.init(this, receiver, &mixer, &ratePid, true);

	// Add optical-flow sensor
	hackflight.addSensor(&_flowSensor);

	// Add rangefinder
	hackflight.addSensor(&_rangefinder);

	// Add level PID controller for aux switch position 1
	hackflight.addPidController(&level, 1);

	// Add loiter PID controllers for aux switch position 2
	hackflight.addPidController(&althold, 2);
	//hackflight.addPidController(&poshold, 2);

    // Initialize the motor-spin values
    for (uint8_t k=0; k<4; ++k) {
        _motorvals[k] = 0;
    }

	// Load our Sound Cue for the propeller sound we created in the editor... 
	// note your path may be different depending
	// on where you store the asset on disk.
	static ConstructorHelpers::FObjectFinder<USoundCue> propellerCue(TEXT("'/Game/Flying/Audio/MotorSoundCue'"));
	
	// Store a reference to the Cue asset - we'll need it later.
	propellerAudioCue = propellerCue.Object;

	// Create an audio component, the audio component wraps the Cue, 
	// and allows us to ineract with it, and its parameters from code.
	propellerAudioComponent = CreateDefaultSubobject<UAudioComponent>(TEXT("PropellerAudioComp"));

	// Stop the sound from sound playing the moment it's created.
	propellerAudioComponent->bAutoActivate = false;

	// Attach the sound to the pawn's root, the sound follows the pawn around
	propellerAudioComponent->SetupAttachment(GetRootComponent());

    // Set up the FPV camera
    fpvSpringArm = CreateDefaultSubobject<USpringArmComponent>(L"FpvSpringArm");
    fpvSpringArm->SetupAttachment(RootComponent);
	fpvSpringArm->TargetArmLength = 0.f; // The camera follows at this distance behind the character
    fpvCamera = CreateDefaultSubobject<UCameraComponent>(L"FpvCamera");
    fpvCamera ->SetupAttachment(fpvSpringArm, USpringArmComponent::SocketName); 
}

void AVehiclePawn::PostInitializeComponents()
{
	if (propellerAudioCue->IsValidLowLevelFast()) {
		propellerAudioComponent->SetSound(propellerAudioCue);
	}

    // Grab the static prop mesh components by name, storing them for use in Tick()
    TArray<UStaticMeshComponent *> staticComponents;
    this->GetComponents<UStaticMeshComponent>(staticComponents);
    for (int i = 0; i < staticComponents.Num(); i++) {
        if (staticComponents[i]) {
            UStaticMeshComponent* child = staticComponents[i];
            if (child->GetName() == "Prop1") PropMeshes[0] = child;
            if (child->GetName() == "Prop2") PropMeshes[1] = child;
            if (child->GetName() == "Prop3") PropMeshes[2] = child;
            if (child->GetName() == "Prop4") PropMeshes[3] = child;
        }
	}

	Super::PostInitializeComponents();
}

void AVehiclePawn::BeginPlay()
{
    // Start playing the sound.  Note that because the Cue Asset is set to loop the sound,
    // once we start playing the sound, it will play continiously...
    propellerAudioComponent->Play();

    // Initialize sensor simulation variables
    _eulerPrev = FVector(0, 0, 0);
	_varioPrev = 0;
	_accelZ = 0;
	_elapsedTime = 1.0; // avoid divide-by-zero

	// Initialize sensors
	//_rangefinder.init();

	// Start the server
    _serverRunning = true;
	if (!server.start()) {
        serverError();
        _serverRunning = false;
    }
	_serverAvailableBytes = 0;

	// Start Python-based loiter if indicated
#ifdef _PYTHON
	loiter.start();
#endif

    Super::BeginPlay();
}

void AVehiclePawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // Stop the server if it has a client connection
    if (_serverRunning) {
        if (server.connected()) {
            if (server.disconnect()) {
                hf::Debug::printf("Disconnected");
            }
            else {
                serverError();
            }
        }
        server.stop();
    }

    Super::EndPlay(EndPlayReason);
}

void AVehiclePawn::Tick(float DeltaSeconds)
{
    hf::Debug::printf("%d FPS", (uint16_t)(1/DeltaSeconds));

    // Poll the joystick, updating the SimReceiver
    joystickPoll();

    // Update our flight firmware
    hackflight.update();

    // Compute body-frame roll, pitch, yaw velocities based on differences between motors
    float forces[3];
    forces[0] = motorsToAngularForce(2, 3, 0, 1);
    forces[1] = motorsToAngularForce(1, 3, 0, 2); 
    forces[2] = motorsToAngularForce(1, 2, 0, 3); 

    // Rotate vehicle
    AddActorLocalRotation(DeltaSeconds * FRotator(forces[1], forces[2], forces[0]) * (180 / M_PI));

    // Spin props proportionate to motor values, acumulating their sum 
    float motorSum = 0;
    for (uint8_t k=0; k<4; ++k) {
        FRotator PropRotation(0, _motorvals[k]*motordirs[k]*60, 0);
        PropMeshes[k]->AddLocalRotation(PropRotation);
        motorSum += _motorvals[k];
    }

    // Get current quaternion
    _quat = this->GetActorQuat();

    // Convert quaternion to Euler angles
    FVector euler = this->getEulerAngles();

    // Use Euler angle first difference to emulate gyro
    _gyro = (euler - _eulerPrev) / DeltaSeconds;
    _eulerPrev = euler;

	// Use velocity first difference to emulate accelerometer
	float vario = this->GetVelocity().Z / 100; // m/s
	_accelZ = (vario - _varioPrev) / DeltaSeconds;
	_varioPrev = vario;

    // Rotate Euler angles into inertial frame: http://www.chrobotics.com/library/understanding-euler-angles
    float x = sin(euler.X)*sin(euler.Z) + cos(euler.X)*cos(euler.Z)*sin(euler.Y);
    float y = cos(euler.X)*sin(euler.Y)*sin(euler.Z) - cos(euler.Z)*sin(euler.X);
    float z = cos(euler.Y)*cos(euler.X);

    // Add movement force to vehicle with simple piecewise nonlinearity
    PlaneMesh->AddForce(motorSum*THRUST_FACTOR*FVector(-x, -y, z));

    // Modulate the pitch and voume of the propeller sound
    propellerAudioComponent->SetFloatParameter(FName("pitch"), motorSum / 4);
    propellerAudioComponent->SetFloatParameter(FName("volume"), motorSum / 4);

    // Debug status of client connection
    if (!server.connected() && _serverRunning) {
        //hf::Debug::printf("Server running but not connected");
    }

    if (server.connected() && _serverRunning) {
        //hf::Debug::printf("Server connected");
    }

    // Call any parent class Tick implementation
    Super::Tick(DeltaSeconds);
}

void AVehiclePawn::NotifyHit(class UPrimitiveComponent* MyComp, class AActor* Other, class UPrimitiveComponent* OtherComp, 
        bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
    Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);

    // Deflect along the surface when we collide.
    FRotator CurrentRotation = GetActorRotation();
    SetActorRotation(FQuat::Slerp(CurrentRotation.Quaternion(), HitNormal.ToOrientationQuat(), 0.025f));
}

float AVehiclePawn::motorsToAngularForce(int a, int b, int c, int d)
{
    float v = ((_motorvals[a] + _motorvals[b]) - (_motorvals[c] + _motorvals[d]));

    return (v<0 ? -1 : +1) * fabs(v);
}

void AVehiclePawn::serverError(void)
{
    hf::Debug::printf("MSP server error: %s", server.lastError());
}

AVehiclePawn::GaussianNoise::GaussianNoise(uint8_t size, float noise)
{
    _size  = size;
    _noise = noise;

    _dist = std::normal_distribution<float>(0, _noise);
}

void AVehiclePawn::GaussianNoise::addNoise(float vals[])
{
    for (uint8_t k=0; k<_size; ++k) {
        vals[k] += _dist(_generator);
    }
}


// Joystick support ---------------------------------------------------------------------

void AVehiclePawn::joystickInit(void)
{
    JOYCAPS joycaps;

    uint8_t axismap[5];
    uint8_t buttonmap[5];

    bool springyThrottle = false;
    bool useButtonForAux = false;
    bool reversedVerticals = false;

    // Grab the first available joystick
    for (_joyid=0; _joyid<16; _joyid++)
        if (joyGetDevCaps(_joyid, &joycaps, sizeof(joycaps)) == JOYERR_NOERROR)
            break;

    if (_joyid < 16) {

        uint16_t vendorId = joycaps.wMid;
        uint16_t productId = joycaps.wPid;

        // axes: 0=Thr 1=Ael 2=Ele 3=Rud 4=Aux
        // JOYINFOEX: 0=dwXpos 1=dwYpos 2=dwZpos 3=dwRpos 4=dwUpos 5=dwVpos

        // R/C transmitter
        if (vendorId == VENDOR_STM) {

            if (productId == PRODUCT_TARANIS) {
                axismap[0] =   0;
                axismap[1] =   1;
                axismap[2] =   2;
                axismap[3] =   5;
                axismap[4] =   3;
            }
            else { // Spektrum
                axismap[0] = 1;
                axismap[1] = 2;
                axismap[2] = 5;
                axismap[3] = 0;
                axismap[4] = 4;
            }
        }

        else {

            reversedVerticals = true;

            switch (productId) {

                case PRODUCT_PS3_CLONE:      
                case PRODUCT_PS4:
                    axismap[0] = 1;
                    axismap[1] = 2;
                    axismap[2] = 3;
                    axismap[3] = 0;
                    springyThrottle = true;
                    useButtonForAux = true;
                    buttonmap[0] = 1;
                    buttonmap[1] = 2;
                    buttonmap[2] = 4;
                    break;


                case PRODUCT_XBOX360_CLONE:
                    axismap[0] = 1;
                    axismap[1] = 4;
                    axismap[2] = 3;
                    axismap[3] = 0;
                    springyThrottle = true;
                    useButtonForAux = true;
                    buttonmap[0] = 8;
                    buttonmap[1] = 2;
                    buttonmap[2] = 1;
                    break;

                case PRODUCT_EXTREMEPRO3D:  
                    axismap[0] = 2;
                    axismap[1] = 0;
                    axismap[2] = 1;
                    axismap[3] = 3;
                    useButtonForAux = true;
                    buttonmap[0] = 1;
                    buttonmap[1] = 2;
                    buttonmap[2] = 4;
                    break;

                case PRODUCT_F310:
                    axismap[0] = 1;
                    axismap[1] = 4;
                    axismap[2] = 3;
                    axismap[3] = 0;
                    springyThrottle = true;
                    useButtonForAux = true;
                    buttonmap[0] = 8;
                    buttonmap[1] = 2;
                    buttonmap[2] = 1;
                    break;
            }
        }
    }

    receiver = new SimReceiver(axismap, buttonmap, reversedVerticals, springyThrottle, useButtonForAux);

} // joystickInit

void AVehiclePawn::joystickPoll(void)
{
    JOYINFOEX joyState;
    joyState.dwSize=sizeof(joyState);
    joyState.dwFlags=JOY_RETURNALL | JOY_RETURNPOVCTS | JOY_RETURNCENTERED | JOY_USEDEADZONE;
    joyGetPosEx(_joyid, &joyState);

    int32_t axes[6];
    axes[0] = joyState.dwXpos;
    axes[1] = joyState.dwYpos;
    axes[2] = joyState.dwZpos;
    axes[3] = joyState.dwRpos;
    axes[4] = joyState.dwUpos;
    axes[5] = joyState.dwVpos;

    uint8_t buttons = joyState.dwButtons;

    receiver->update(axes, buttons);
}


// Hackflight::Board methods ---------------------------------------------------

bool AVehiclePawn::getQuaternion(float q[4]) 
{   
    q[0] = +_quat.W;
    q[1] = -_quat.X;
    q[2] = -_quat.Y;
    q[3] = +_quat.Z;

    _quatNoise.addNoise(q);

    return true;
}

bool AVehiclePawn::getGyrometer(float gyroRates[3])
{
    gyroRates[0] = _gyro.X;
    gyroRates[1] = _gyro.Y;
    gyroRates[2] = 0; // _gyro.Z; // XXX zero-out gyro Z (yaw) for now

    //_gyroSensor.addNoise(gyroRates);

    return true;
}

void AVehiclePawn::writeMotor(uint8_t index, float value) 
{
    _motorvals[index] = value;
}

float AVehiclePawn::getTime(void)
{
    // Track elapsed time
    _elapsedTime += .01; // Assume 100Hz clock

    return _elapsedTime;
}

uint8_t AVehiclePawn::serialAvailableBytes(void)
{ 
    if (_serverAvailableBytes > 0) {
        return _serverAvailableBytes;
    }

    else if (server.connected()) {
        _serverAvailableBytes = server.receiveBuffer(_serverBuffer, ThreadedSocketServer::BUFLEN);
        if (_serverAvailableBytes < 0) {
            _serverAvailableBytes = 0;
        }
        _serverByteIndex = 0;
        return _serverAvailableBytes;
    }

    return 0;
}

uint8_t AVehiclePawn::serialReadByte(void)
{ 
    uint8_t byte = _serverBuffer[_serverByteIndex]; // post-increment 
    _serverByteIndex++;
    _serverAvailableBytes--;
    return byte;
}

void AVehiclePawn::serialWriteByte(uint8_t c)
{ 
    if (server.connected()) {
        server.sendBuffer((char *)&c, 1);
    }
}



// Helper methods ---------------------------------------------------------------------------------

FVector AVehiclePawn::getEulerAngles(void)
{
    return FMath::DegreesToRadians(this->GetActorQuat().Euler());
}
