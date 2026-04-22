
#include <cstdlib>
#include "XMLFile.h"
#include "ResourceCache.h"
#include "Console.h"
#include "UI.h"
#include "CoreEvents.h"
#include "Engine.h"
#include "DebugHud.h"
#include "Input.h"
#include "InputEvents.h"
#include "Log.h"
#include "Texture2D.h"
#include "UI.h"
#include "Sprite.h"
#include "TBEClientApp.h"
#include "../Refresh/TBEModelLoad.h"

extern "C"
{
 #include "../../client/client.h"
}

 // Force flush of static caches to ensure Urho3D Context is valid when we release materials
// This is registered with atexit to run even if Quake calls exit() directly
void ForceFlushAllCaches(void);

// Translate Urho3D keycode to Quake 2 key
int UrhoToQuakeKey(int urhoKey)
{
    // === SPECIAL KEYS FIRST ===
    switch (urhoKey)
    {
        case 96:            return 96;   // Grave / console toggle
        case 13:            return 13;   // Enter key
        case 8:             return 127;  // Backspace
        // Add other raw keycodes as needed
        default:            break;
    }

    // === ASCII PRINTABLE SECOND ===
    // Lowercase letters
    if (urhoKey >= 'a' && urhoKey <= 'z')
        return urhoKey;
    // Uppercase letters (convert to lowercase for Quake)
    if (urhoKey >= 'A' && urhoKey <= 'Z')
        return urhoKey + ('a' - 'A');
    // Numbers
    if (urhoKey >= '0' && urhoKey <= '9')
        return urhoKey;
    // Space
    if (urhoKey == ' ')
        return 32;  // K_SPACE

    // === UNKNOWN ===
    return -1;
}

DEFINE_APPLICATION_MAIN(TBEClientApp)

TBEClientApp::TBEClientApp(Context* context) : TBEApp(context)
{

}

/// Setup before engine initialization. Modifies the engine parameters.
void TBEClientApp::Setup()
{
    // Modify engine startup parameters
    engineParameters_["WindowTitle"] = "QuakeToon";
    engineParameters_["LogName"]     = "QuakeToon.log";
    engineParameters_["FullScreen"]  = false;
    engineParameters_["Headless"]    = false;
    engineParameters_["WindowWidth"]    = 1280;
    engineParameters_["WindowHeight"]    = 720;
    engineParameters_["ResourcePaths"] = "Data;CoreData;Extra";
}

/// Setup after engine initialization. Creates the logo, console & debug HUD.
void TBEClientApp::Start()
{

    // Disable OS cursor
    // GetSubsystem<Input>()->SetMouseVisible(false);

    // Get default style
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    XMLFile* xmlFile = cache->GetResource<XMLFile>("UI/DefaultStyle.xml");

    // Do not create Urho3D's console to avoid conflicts with Quake's console
    // Console* console = engine_->CreateConsole();
    // console->SetDefaultStyle(xmlFile);
    // console->GetBackground()->SetOpacity(0.8f);

    // Log Urho3D version for verification
    Log::Write(LOG_INFO, "Urho3D Version Check - Context initialized");

    // Create debug HUD.
    DebugHud* debugHud = engine_->CreateDebugHud();
    debugHud->SetDefaultStyle(xmlFile);
    debugHud->Toggle(DEBUGHUD_SHOW_ALL);

    CreateLogo();

    // todo, define argc and argv as Urho3D also wants command line args
    //int argc = 5;
    //const char *argv[] = {"quake", "+map", "demo1", "+notarget", "+god"};

    int argc = 3;
    const char *argv[] = {"quake", "+map", "base1"};

    Qcommon_Init (argc, (char**) argv);

    // Bind F key to fire weapon
    Cmd_ExecuteString("bind f +attack");

    // Finally subscribe to the update event. Note that by subscribing events at this point we have already missed some events
    // like the ScreenMode event sent by the Graphics subsystem when opening the application window. To catch those as well we
    // could subscribe in the constructor instead.
    SubscribeToEvents();
}

void TBEClientApp::CreateLogo()
{
    // Get logo texture
   ResourceCache* cache = GetSubsystem<ResourceCache>();
   Texture2D* logoTexture = cache->GetResource<Texture2D>("Textures/LogoLarge.png");
   if (!logoTexture)
       return;

   // Create logo sprite and add to the UI layout
   UI* ui = GetSubsystem<UI>();
   logoSprite_ = ui->GetRoot()->CreateChild<Sprite>();

   // Set logo sprite texture
   logoSprite_->SetTexture(logoTexture);

   int textureWidth = logoTexture->GetWidth();
   int textureHeight = logoTexture->GetHeight();

   // Set logo sprite scale
   logoSprite_->SetScale(256.0f / textureWidth);

   // Set logo sprite size
   logoSprite_->SetSize(textureWidth, textureHeight);

   // Set logo sprite hot spot
   logoSprite_->SetHotSpot(0, textureHeight);

   // Set logo sprite alignment
   logoSprite_->SetAlignment(HA_LEFT, VA_BOTTOM);

   // Make logo not fully opaque to show the scene underneath
   logoSprite_->SetOpacity(0.75f);

   // Set a low priority for the logo so that other UI elements can be drawn on top
   logoSprite_->SetPriority(-100);
}

void TBEClientApp::SubscribeToEvents()
{
    SubscribeToEvent(E_UPDATE, HANDLER(TBEClientApp, HandleUpdate));

    SubscribeToEvent(E_KEYDOWN, HANDLER(TBEClientApp, HandleKeyDown));
    SubscribeToEvent(E_KEYUP, HANDLER(TBEClientApp, HandleKeyUp));

    SubscribeToEvent(E_TEXTINPUT, HANDLER(TBEClientApp, HandleTextInput));
}

void TBEClientApp::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace Update;
    float timeStep = eventData[P_TIMESTEP].GetFloat() * 1000.0f;

    Input* input = GetSubsystem<Input>();

    // Movement speed as world units per second
    const float MOVE_SPEED = 20.0f;
    // Mouse sensitivity as degrees per pixel
    const float MOUSE_SENSITIVITY = 0.2f;

    // Use this frame's mouse motion to adjust camera node yaw and pitch. Clamp the pitch between -90 and 90 degrees
    IntVector2 mouseMove = input->GetMouseMove();

    cl.viewangles[YAW] -= MOUSE_SENSITIVITY * mouseMove.x_;
    cl.viewangles[PITCH] += MOUSE_SENSITIVITY * mouseMove.y_;

    // Clamp pitch so you don't look backwards or at your feet
    if (cl.viewangles[PITCH] > 89.0f)
        cl.viewangles[PITCH] = 89.0f;
    if (cl.viewangles[PITCH] < -89.0f)
        cl.viewangles[PITCH] = -89.0f;

    // Construct new orientation for the camera scene node from yaw and pitch. Roll is fixed to zero
    //cameraNode_->SetRotation(Quaternion(pitch_, yaw_, 0.0f));

    // Read WASD keys and move the camera scene node to the corresponding direction if they are pressed
    // Use the Translate() function (default local space) to move relative to the node's orientation.
    /*
       if (input->GetKeyDown('W'))
           cameraNode_->Translate(Vector3::FORWARD * MOVE_SPEED * timeStep);
       if (input->GetKeyDown('S'))
           cameraNode_->Translate(Vector3::BACK * MOVE_SPEED * timeStep);
       if (input->GetKeyDown('A'))
           cameraNode_->Translate(Vector3::LEFT * MOVE_SPEED * timeStep);
       if (input->GetKeyDown('D'))
           cameraNode_->Translate(Vector3::RIGHT * MOVE_SPEED * timeStep);
       */

    static bool wdown = false;
    static bool sdown = false;
    static bool adown = false;
    static bool ddown = false;
    static bool cdown = false;
    static bool fdown = false;
    static bool spacedown = false;
    static bool consdown = false;

    if (input->GetKeyDown('W') && !wdown)
    {
        wdown = true;

        Key_Event(K_UPARROW, qtrue, 0);

    }
    else if (wdown)
    {
        wdown = false;

        Key_Event(K_UPARROW, qfalse, 0);
    }

    if (input->GetKeyDown('S') && !sdown)
    {
        sdown = true;

        Key_Event(K_DOWNARROW, qtrue, 0);

    }
    else if (sdown)
    {
        sdown = false;

        Key_Event(K_DOWNARROW, qfalse, 1);
    }

    if (input->GetKeyDown('A') && !adown)
    {
        adown = true;

        Key_Event(K_LEFTARROW, qtrue, 0);

    }
    else if (adown)
    {
        adown = false;

        Key_Event(K_LEFTARROW, qfalse, 0);
    }

    if (input->GetKeyDown('D') && !ddown)
    {
        ddown = true;

        Key_Event(K_RIGHTARROW, qtrue, 0);

    }
    else if (ddown)
    {
        ddown = false;

        Key_Event(K_RIGHTARROW, qfalse, 0);
    }

    if (input->GetKeyDown(' ') && !spacedown)
    {
        spacedown = true;

        Key_Event(K_SPACE, qtrue, 0);

    }
    else if (spacedown)
    {
        spacedown = false;

        Key_Event(K_SPACE, qfalse, 0);
    }

    if (input->GetKeyDown('`') && !consdown)
    {
        consdown = true;

        Key_Event('`', qtrue, 0);
    }
    else if (consdown)
    {
        consdown = false;

        Key_Event('`', qfalse, 0);
    }

    if (input->GetKeyDown('C') && !cdown)
    {
        cdown = true;

        Key_Event('c', qtrue, 0);

    }
    else if (cdown)
    {
        cdown = false;

        Key_Event('c', qfalse, 0);
    }

    if (input->GetKeyDown('F') && !fdown)
    {
        fdown = true;

        Key_Event('f', qtrue, 0);

    }
    else if (fdown)
    {
        fdown = false;

        Key_Event('f', qfalse, 0);
    }

    Qcommon_Frame((int) timeStep);
}

// Wrapper to allow C code (Quake) to call our C++ cleanup
extern "C" void Com_Shutdown_Caches(void)
{
    Mod_AliasModel_Shutdown();
    Mod_MapModel_Shutdown();
}

void TBEClientApp::Stop()
{
    // Ensure Urho3D resources are released while Context is valid
    Com_Shutdown_Caches();

    // Shutdown Quake subsystems BEFORE Urho3D destroys the engine
    // This prevents Quake from trying to access Urho3D subsystems that are already gone
    Qcommon_Shutdown();

    // Clear our UI references before base class destroys the engine
    logoSprite_.Reset();

    // Call base class stop
    TBEApp::Stop();
}

void TBEClientApp::HandleKeyDown(StringHash eventType, VariantMap& eventData)
{
    using namespace KeyDown;
    int urhoKey = eventData[P_KEY].GetInt();
    int qkey = UrhoToQuakeKey(urhoKey);

    // DEBUG: print every keypress so we can see what ~ sends
    Log::Write(LOG_INFO, String("KEYDOWN: raw=") + String(urhoKey) +
               " translated=" + String(qkey));

    if (qkey >= 0)
    {
        Log::Write(LOG_INFO, String("  -> Sending to Quake: ") + String(qkey));
        Key_Event(qkey, qtrue, 0);
    }
    else
    {
        Log::Write(LOG_INFO, "  -> Ignored (unmapped)");
    }
}

void TBEClientApp::HandleKeyUp(StringHash eventType, VariantMap& eventData)
{
    using namespace KeyUp;
    int urhoKey = eventData[P_KEY].GetInt();
    int qkey = UrhoToQuakeKey(urhoKey);

    if (qkey >= 0)
        Key_Event(qkey, qfalse, 0);
}

void TBEClientApp::HandleTextInput(StringHash eventType, VariantMap& eventData)
{
    using namespace TextInput;

    // Get the text string (character) typed by the user
    String text = eventData[P_TEXT].GetString();

    if (!text.Empty())
    {
        // Pass the character to Quake 2's console input
        // We iterate because Urho3D might send multiple chars (paste), though typing is usually 1 by 1
        for (unsigned i = 0; i < text.Length(); i++)
        {
            Char_Event(text[i]);
        }
    }
}

