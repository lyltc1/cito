#include <stdio.h>
#include <fstream>

#include "mujoco.h"
#include "glfw3.h"

#include "cito_params.h"

/// global variables
YAML::Node paths;

const int maxgeom= 5000;

// model and data
mjModel* m = 0;
mjData* d = 0;
float* data = 0;
int recsz = 0;
long long numrec = 0;
long long frame = 0;
mjtNum timestep = 0;
float* rgb = 0;
mjtNum* pointxy = 0;
int* npoints = 0;

// user state
bool paused = false;
bool jumped = false;
bool showoption = false;
bool showinfo = true;
bool showsensor = true;
bool showfullscreen = false;
int showhelp = 0;                   // 0: none; 1: brief; 2: full

// abstract visualization
mjvScene scn;
mjvCamera cam;
mjvOption vopt;
char status[1000] = "";
mjvFigure figsensor;

// OpenGL rendering
GLFWwindow* window = 0;
int refreshrate;
mjrContext con;

// selection and perturbation
bool button_left = false;
bool button_middle = false;
bool button_right =  false;
bool reposition = false;
double lastx = 0;
double lasty = 0;
int needselect = 0;                 // 0: none; 2: center; 3: center and track
double window2buffer = 1;           // framebuffersize / windowsize (for scaled video modes)
double fontscale = 1.5;

// help strings
const char help_title[] =
"Help\n"
"Option\n"
"Info\n"
"Sensor\n"
"Full screen\n"
"Pause\n"
"Forward\n"
"Back\n"
"Forward 100\n"
"Back 100\n"
"Start\n"
"End\n"
"Geoms\n"
"Sites\n"
"Zoom\n"
"Translate\n"
"Rotate\n"
"Track\n"
"Center\n"
"Free camera\n"
"Camera\n"
"Frame\n"
"Label";


const char help_content[] =
"F1\n"
"F2\n"
"F3\n"
"F4\n"
"F5\n"
"Space\n"
"Down arrow\n"
"Up arrow\n"
"Right arrow\n"
"Left arrow\n"
"Home\n"
"End\n"
"0 - 4\n"
"Shift 0 - 4\n"
"Scroll or M drag\n"
"[Shift] R drag\n"
"L drag\n"
"L dbl click\n"
"R dbl click\n"
"Esc\n"
"[ ]\n"
"; '\n"
". /";

char opt_title[1000] = "";
char opt_content[1000];


/// initialize GLFW and OpenGL
void initOpenGL(const char* modelFilePath, const char* logFilePath)
{
    // init GLFW
    if (!glfwInit())
        return;

    // get refreshrate, set multisampling
    refreshrate = glfwGetVideoMode(glfwGetPrimaryMonitor())->refreshRate;
    glfwWindowHint(GLFW_SAMPLES, 4);

    // create window
    // window size modification ************************************************
    window = glfwCreateWindow(1280, 800, "MuJoCo Playlog", NULL, NULL);
    if( !window )
    {
        glfwTerminate();
        return;
    }

    // make context current, request v-sync on swapbuffers
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // save window-to-framebuffer pixel scaling (needed for OSX scaling)
    int width, width1, height;
    glfwGetWindowSize(window, &width, &height);
    glfwGetFramebufferSize(window, &width1, &height);
    window2buffer = (double)width1 / (double)width;

    // remove path info from file names
    size_t n1, n2;
    for( n1=strlen(modelFilePath)-1; n1>0; n1-- )
        if( modelFilePath[n1]=='\\' || modelFilePath[n1]=='/' )
        {
            n1++;
            break;
        }
    for( n2=strlen(logFilePath)-1; n2>0; n2-- )
        if( logFilePath[n2]=='\\' || logFilePath[n2]=='/' )
        {
            n2++;
            break;
        }

    // make title and set
    char title[500];
    sprintf(title, "MuJoCo Playlog:  %s  %s", modelFilePath+n1, logFilePath+n2);
    glfwSetWindowTitle(window, title);
}


/// set one frame, from global frame counter
void setFrame(void)
{
    d->time = (mjtNum)data[recsz*frame];
    mju_f2n(d->qpos, data+recsz*frame+1, m->nq);
    mju_f2n(d->qvel, data+recsz*frame+1+m->nq, m->nv);
    mju_f2n(d->ctrl, data+recsz*frame+1+m->nq+m->nv, m->nu);
    mju_f2n(d->mocap_pos, data+recsz*frame+1+m->nq+m->nv+m->nu, 3*m->nmocap);
    mju_f2n(d->mocap_quat, data+recsz*frame+1+m->nq+m->nv+m->nu+3*m->nmocap, 4*m->nmocap);
    mju_f2n(d->sensordata, data+recsz*frame+1+m->nq+m->nv+m->nu+7*m->nmocap, m->nsensordata);
}


/// load model, init simulation and rendering
void initMuJoCo(const char* modelFilePath, const char* logFilePath)
{
    // activate MuJoCo license
    const char* mjKeyPath = std::getenv("MJ_KEY");
    mj_activate(mjKeyPath);
    // load and compile model
    if( strlen(modelFilePath)>4 && !strcmp(modelFilePath+strlen(modelFilePath)-4, ".mjb") )
         m = mj_loadModel(modelFilePath, NULL);
    else m = mj_loadXML(modelFilePath, NULL, NULL, 0);
    if( !m ) mju_error("ERROR:  Model cannot be loaded");
    // copy timestep, adjust later
    timestep = m->opt.timestep;
    // open log file
    FILE* logFile = fopen(logFilePath, "rb");
    if( !logFile )
        mju_error("Unable to open the log file.");
    // get header with sizes, check
    int header[6];
    if( fread(header, sizeof(int), 6, logFile) != 6 )
        mju_error("Could not read log file header");
    if( m->nq!=header[0] || m->nv!=header[1] || m->nu!=header[2] ||
        m->nmocap!=header[3] || m->nsensordata!=header[4] )
          mju_error("Model sizes incompatible with sizes found in log file header");
    // compute record size
    recsz = 1 + m->nq + m->nv + m->nu + 7*m->nmocap + m->nsensordata;
    long long startpos = ftello(logFile);
    fseeko(logFile, 0, SEEK_END);
    long long filesz = ftello(logFile) - startpos;
    fseeko(logFile, startpos, SEEK_SET);
    numrec = filesz/recsz/sizeof(float);
    // allocate buffers
    data = (float*) malloc(filesz);
    if( !data )
        mju_error("Could not allocate memory buffer for log file data");
    if( m->nsensordata )
    {
        rgb = (float*) malloc(sizeof(float)*3*(5+m->nsensordata));
        pointxy = (mjtNum*) malloc(sizeof(mjtNum)*4*(5+m->nsensordata));
        npoints = (int*) malloc(sizeof(int)*(5+m->nsensordata));
        if( !pointxy || !npoints )
            mju_error("Could not allocate memory buffer for 2d plot");
    }

    // read data, print size
    size_t nn = fread(data, recsz*sizeof(float), numrec, logFile);
    if( nn!=numrec )
        mju_error("Unexpected amount of data read");
    fclose(logFile);
    printf("Loaded %lld data frames from log file\n\n", numrec);

    // make data, set first frame
    d = mj_makeData(m);
    frame = 0;
    setFrame();
    mj_forward(m, d);

    // initialize MuJoCo visualization
    mjv_makeScene(m, &scn, maxgeom);
    mjv_defaultCamera(&cam);
    mjv_defaultOption(&vopt);
    mjr_defaultContext(&con);
    mjr_makeContext(m, &con, (int)(100*fontscale));

    // center and scale view
    cam.lookat[0] = m->stat.center[0];
    cam.lookat[1] = m->stat.center[1];
    cam.lookat[2] = m->stat.center[2];
    cam.distance = 1.5 * m->stat.extent;
    cam.type = mjCAMERA_FREE;
}


/// deallocate everything
void closeMuJoCo(void)
{
    free(npoints);
    free(pointxy);
    free(rgb);
    free(data);
    mj_deleteData(d);
    mj_deleteModel(m);
    mjr_freeContext(&con);
    mjv_freeScene(&scn);

    m = 0;
}


/// bar dimensions
typedef struct
{
    int width;          // framebuffer width (bar is from 1/4 to 3/4 width)
    int bar;            // bar height
    int cpos;           // cursor position
    int cwidth;         // cursor width
    int cheight;        // cursor height
    int lwidth;         // line width
} Bar;


/// compute scroll bar sizes
Bar getBar(int width)
{
    Bar b;

    b.width = width;
    b.bar = (int)(40*fontscale);
    b.cpos = (int)(width/2*(double)frame/(double)numrec);
    b.cwidth = (int)(10*fontscale);
    b.cheight = (int)(20*fontscale);
    b.lwidth = (int)(4*fontscale);

    return b;
}


/// reposition frame based on mouse horizontal position
void repositionFrame(Bar b, int x)
{
    double relpos = (double)(x-b.width/4) / (double)(b.width/2);
    frame = (long long) (numrec * relpos);
    if( frame<0 )
        frame = 0;
    else if( frame>numrec-1 )
        frame = numrec-1;

    setFrame();
    jumped = true;
}


///GLFW callbacks
// keyboard
void keyboard(GLFWwindow* window, int key, int scancode, int act, int mods)
{
    if( !m )
        return;

    // do not act on release
    if( act==GLFW_RELEASE )
        return;

    switch( key )
    {
    case GLFW_KEY_F1:                   // help
        showhelp++;
        if( showhelp>2 )
            showhelp = 0;
        break;

    case GLFW_KEY_F2:                   // option
        showoption = !showoption;
        break;

    case GLFW_KEY_F3:                   // info
        showinfo = !showinfo;
        break;

    case GLFW_KEY_F4:                   // sensor
        showsensor = !showsensor;
        break;

    case GLFW_KEY_F5:                   // toggle full screen
        showfullscreen = !showfullscreen;
        if( showfullscreen )
            glfwMaximizeWindow(window);
        else
            glfwRestoreWindow(window);
        break;

    case GLFW_KEY_SPACE:                // pause
        paused = !paused;
        break;

    case GLFW_KEY_RIGHT:                // step forward
        frame = mjMIN(frame+1, numrec-1);
        jumped = true;
        setFrame();
        break;

    case GLFW_KEY_LEFT:                 // step back
        frame = mjMAX(frame-1, 0);
        jumped = true;
        setFrame();
        break;

    case GLFW_KEY_DOWN:                 // step forward 100
        frame = mjMIN(frame+100, numrec-1);
        jumped = true;
        setFrame();
        break;

    case GLFW_KEY_UP:                   // step back 100
        frame = mjMAX(frame-100, 0);
        jumped = true;
        setFrame();
        break;

    case GLFW_KEY_HOME:                 // start
        frame  = 0;
        jumped = true;
        setFrame();
        break;

    case GLFW_KEY_END:                  // end
        frame = numrec-1;
        jumped = true;
        setFrame();
        break;

    case GLFW_KEY_ESCAPE:               // free camera
        cam.type = mjCAMERA_FREE;
        break;

    case '[':                           // previous fixed camera or free
        if( m->ncam && cam.type==mjCAMERA_FIXED )
        {
            if( cam.fixedcamid>0 )
                cam.fixedcamid--;
            else
                cam.type = mjCAMERA_FREE;
        }
        break;

    case ']':                           // next fixed camera
        if( m->ncam )
        {
            if( cam.type!=mjCAMERA_FIXED )
            {
                cam.type = mjCAMERA_FIXED;
                cam.fixedcamid = 0;
            }
            else if( cam.fixedcamid<m->ncam-1 )
                cam.fixedcamid++;
        }
        break;

    case ';':                           // cycle over frame rendering modes
        vopt.frame = mjMAX(0, vopt.frame-1);
        break;

    case '\'':                          // cycle over frame rendering modes
        vopt.frame = mjMIN(mjNFRAME-1, vopt.frame+1);
        break;

    case '.':                           // cycle over label rendering modes
        vopt.label = mjMAX(0, vopt.label-1);
        break;

    case '/':                           // cycle over label rendering modes
        vopt.label = mjMIN(mjNLABEL-1, vopt.label+1);
        break;

    default:                            // toggle flag
        // toggle visualization flag
        for( int i=0; i<mjNVISFLAG; i++ )
            if( key==mjVISSTRING[i][2][0] )
                vopt.flags[i] = !vopt.flags[i];

        // toggle rendering flag
        for( int i=0; i<mjNRNDFLAG; i++ )
            if( key==mjRNDSTRING[i][2][0] )
                scn.flags[i] = !scn.flags[i];

        // toggle geom/site group
        for( int i=0; i<mjNGROUP; i++ )
            if( key==i+'0')
            {
                if( mods & GLFW_MOD_SHIFT )
                    vopt.sitegroup[i] = !vopt.sitegroup[i];
                else
                    vopt.geomgroup[i] = !vopt.geomgroup[i];
            }
    }
}


// mouse button
void mouse_button(GLFWwindow* window, int button, int act, int mods)
{
    if( !m )
        return;

    // past data for double-click detection
    static int lastbutton = 0;
    static double lastclicktm = 0;

    // update button state
    button_left =   (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS);
    button_middle = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE)==GLFW_PRESS);
    button_right =  (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT)==GLFW_PRESS);

    // update mouse position
    glfwGetCursorPos(window, &lastx, &lasty);

    // determine bar sizes
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    Bar b = getBar(width);

    // detect repositioning
    if( act==GLFW_PRESS && (int)(height-lasty*window2buffer) < b.bar )
    {
        reposition = true;
        repositionFrame(b, (int)(lastx*window2buffer));
    }
    else
        reposition = false;

    // detect double-click (250 msec)
    if( act==GLFW_PRESS && glfwGetTime()-lastclicktm<0.25 &&
        button==lastbutton && !reposition )
    {
        if( button_right )
            needselect = 2;
        else
            needselect = 3;
    }

    // save info
    if( act==GLFW_PRESS )
    {
        lastbutton = button;
        lastclicktm = glfwGetTime();
    }
}


// mouse move
void mouse_move(GLFWwindow* window, double xpos, double ypos)
{
    if( !m )
        return;

    // no buttons down: nothing to do, clear reposition
    if( !button_left && !button_middle && !button_right )
    {
        reposition = false;
        return;
    }

    // compute mouse displacement, save
    double dx = xpos - lastx;
    double dy = ypos - lasty;
    lastx = xpos;
    lasty = ypos;

    // get current window size
    int width, height;
    glfwGetWindowSize(window, &width, &height);

    // get shift key state
    bool mod_shift = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS ||
                      glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT)==GLFW_PRESS);

    // determine action based on mouse button
    mjtMouse action;
    if( button_right )
        action = mod_shift ? mjMOUSE_MOVE_H : mjMOUSE_MOVE_V;
    else if( button_left )
        action = mod_shift ? mjMOUSE_ROTATE_H : mjMOUSE_ROTATE_V;
    else
        action = mjMOUSE_ZOOM;

    // reposition or move camera
    if( reposition )
    {
        Bar b = getBar((int)(width*window2buffer));
        repositionFrame(b, (int)(lastx*window2buffer));
    }
    else
        mjv_moveCamera(m, action, dx/height, dy/height, &scn, &cam);
}


// scroll
void scroll(GLFWwindow* window, double xoffset, double yoffset)
{
    if( !m )
        return;

    // emulate vertical mouse motion = 5% of window height
    mjv_moveCamera(m, mjMOUSE_ZOOM, 0, -0.05*yoffset, &scn, &cam);
}


/// simulation and rendering
// make option string
void makeoptionstring(const char* name, char key, char* buf)
{
    int i=0, cnt=0;

    // copy non-& characters
    while( name[i] && i<50 )
    {
        if( name[i]!='&' )
            buf[cnt++] = name[i];

        i++;
    }

    // finish
    buf[cnt] = ' ';
    buf[cnt+1] = '(';
    buf[cnt+2] = key;
    buf[cnt+3] = ')';
    buf[cnt+4] = 0;
}


// advance simulation
void simulation(void)
{
    static double lastupdate = glfwGetTime();

    if( glfwGetTime()-lastupdate > m->opt.timestep )
    {
        if( frame<numrec-1 )
        {
            frame++;
            setFrame();
        }

        lastupdate = glfwGetTime();
    }
}


// init sensors figure
static void InitSensor(void)
{
    // set figure to default
    mjv_defaultFigure(&figsensor);

    // set flags
    figsensor.flg_extend = 1;
    figsensor.flg_barplot = 1;

    // title
    strcpy(figsensor.title, "Sensor data");

    // y-tick nubmer format
    strcpy(figsensor.yformat, "%.0f");

    // grid size
    figsensor.gridsize[0] = 2;
    figsensor.gridsize[1] = 3;

    // minimum range
    figsensor.range[0][0] = 0;
    figsensor.range[0][1] = 0;
    figsensor.range[1][0] = -1;
    figsensor.range[1][1] = 1;
}


// update sensors
static void UpdateSensor(void)
{
    static const int maxline = 10;

    // clear linepnt
    for( int i=0; i<maxline; i++ )
        figsensor.linepnt[i] = 0;

    // start with line 0
    int lineid = 0;

    // loop over sensors
    for( int n=0; n<m->nsensor; n++ )
    {
        // go to next line if type is different
        if( n>0 && m->sensor_type[n]!=m->sensor_type[n-1] )
            lineid = mjMIN(lineid+1, maxline-1);

        // get info about this sensor
        mjtNum cutoff = (m->sensor_cutoff[n]>0 ? m->sensor_cutoff[n] : 1);
        int adr = m->sensor_adr[n];
        int dim = m->sensor_dim[n];

        // data pointer in line
        int p = figsensor.linepnt[lineid];

        // fill in data for this sensor
        for( int i=0; i<dim; i++ )
        {
            // check size
            if( (p+2*i)>=mjMAXLINEPNT/2 )
                break;

            // x
            figsensor.linedata[lineid][2*p+4*i] = (float)(adr+i);
            figsensor.linedata[lineid][2*p+4*i+2] = (float)(adr+i);

            // y
            figsensor.linedata[lineid][2*p+4*i+1] = 0;
            figsensor.linedata[lineid][2*p+4*i+3] = (float)(d->sensordata[adr+i]/cutoff);
        }

        // update linepnt
        figsensor.linepnt[lineid] = mjMIN(mjMAXLINEPNT-1,
                                          figsensor.linepnt[lineid]+2*dim);
    }
}



// show sensors
static void ShowSensor(mjrRect rect)
{
    // render figure on the right
    mjrRect viewport = {rect.width - rect.width/4, rect.bottom, rect.width/4, rect.height/3};
    mjr_figure(viewport, &figsensor, &con);
}



// render
void render(GLFWwindow* window)
{
    static double lastrender = 0;

    if( !m )
        return;

    // timing statistics
    if( lastrender==0 )
        lastrender = glfwGetTime();
    int nstep = mjMAX(1, (int)((glfwGetTime()-lastrender)/m->opt.timestep));
    lastrender = glfwGetTime();

    // advance frame to keep realtime
    if( !paused && !jumped && !reposition )
    {
        frame = mjMIN(frame+nstep, numrec-1);
        setFrame();
    }
    mj_forward(m, d);

    // clear flag, so next time we advance automatically
    jumped = false;

    std::ofstream outFile;
    outFile.open(paths["trajFile"].as<std::string>(), std::fstream::app);
    // write to outFile
    if( outFile.is_open() )
    {
      outFile << d->time << ",";
      for( int i=0; i<NU; i++ )
      {
          outFile << d->qpos[params::jact[i]] << ",";
      }
      for( int i=0; i<NU; i++ )
      {
          outFile << d->qvel[params::jact[i]] << ",";
      }
      for( int i=0; i<NU; i++ )
      {
          outFile << d->qacc[params::jact[i]] << ",";
      }
      for( int i=0; i<NU; i++ )
      {
          outFile << d->ctrl[i] << ",";
      }
      outFile <<"\n";
      outFile.close();
    }
    else { std::cout << "Unable to open the trajectory file." << '\n'; }
    // camera string
    char camstr[20];
    if( cam.type==mjCAMERA_FREE )
        strcpy(camstr, "Free");
    else if( cam.type==mjCAMERA_TRACKING )
        strcpy(camstr, "Tracking");
    else
        sprintf(camstr, "Fixed %d", cam.fixedcamid);

    // status
    sprintf(status, "%-20.4f\n%d (%d)\n%s\n%s\n%s",
            d->time,
            d->nefc,
            d->ncon,
            camstr,
            mjFRAMESTRING[vopt.frame],
            mjLABELSTRING[vopt.label]);

    // get current framebuffer rectangle
    mjrRect rectfull = {0, 0, 0, 0};
    glfwGetFramebufferSize(window, &rectfull.width, &rectfull.height);

    // get scroll bar sizes, remove from rendering
    Bar b = getBar(rectfull.width);
    mjrRect rect = {0, b.bar, rectfull.width, rectfull.height-b.bar};

    // get window size (different from framebuffer size)
    mjrRect R = {0, 0, 0, 0};
    glfwGetWindowSize(window, &R.width, &R.height);
    R.bottom = mju_round(b.bar/window2buffer);
    R.height -= R.bottom;

    // selection
    if( needselect )
    {
        // find selected model geom and body
        mjtNum pos[3];
        mjtNum selpnt[3];
        int selskin;
        int selgeom = mjv_select(m, d, &vopt,
                                 (mjtNum)R.width/(mjtNum)R.height,
                                 (mjtNum)lastx/(mjtNum)R.width,
                                 (mjtNum)(R.height-lasty)/(mjtNum)R.height,
                                 &scn, selpnt, &selgeom, &selskin);
        int selbody = (selgeom>=0 ? m->geom_bodyid[selgeom] : 0);

        // set lookat point
        if( selgeom>=0 )
            mju_copy3(cam.lookat, pos);

        // switch to tracking camera
        if( needselect==3 && selbody )
        {
            cam.type = mjCAMERA_TRACKING;
            cam.trackbodyid = selbody;
            cam.fixedcamid = -1;
        }

        needselect = 0;
    }

    // update scene
    mjv_updateScene(m, d, &vopt, NULL, &cam, mjCAT_ALL, &scn);

    // render
    mjr_render(rect, &scn, &con);

    // show help
    if( showhelp==1 )
        mjr_overlay(mjFONT_NORMAL, mjGRID_TOPLEFT, rect, "Help  ", "F1  ", &con);
    else if( showhelp==2 )
        mjr_overlay(mjFONT_NORMAL, mjGRID_TOPLEFT, rect, help_title, help_content, &con);

    // show info
    sprintf(status, "%-5.4f\n%d", d->time, d->ncon);
    if( showinfo )
        mjr_overlay(mjFONT_NORMAL, mjGRID_BOTTOMLEFT, rect,
                    "Time\nContacts", status, &con);
//        mjr_overlay(mjFONT_NORMAL, mjGRID_BOTTOMLEFT, rect,
//                    "Time\nSize\nCamera\nFrame\nLabel", status, &con);

    // show options
    if( showoption )
    {
        int i;
        char buf[100];

        // fill titles on first pass
        if( !opt_title[0] )
        {
            for( i=0; i<mjNRNDFLAG; i++)
            {
                makeoptionstring(mjRNDSTRING[i][0], mjRNDSTRING[i][2][0], buf);
                strcat(opt_title, buf);
                strcat(opt_title, "\n");
            }
            for( i=0; i<mjNVISFLAG; i++)
            {
                makeoptionstring(mjVISSTRING[i][0], mjVISSTRING[i][2][0], buf);
                strcat(opt_title, buf);
                if( i<mjNVISFLAG-1 )
                    strcat(opt_title, "\n");
            }
        }

        // fill content
        opt_content[0] = 0;
        for( i=0; i<mjNRNDFLAG; i++)
        {
            strcat(opt_content, scn.flags[i] ? " + " : "   ");
            strcat(opt_content, "\n");
        }
        for( i=0; i<mjNVISFLAG; i++)
        {
            strcat(opt_content, vopt.flags[i] ? " + " : "   ");
            if( i<mjNVISFLAG-1 )
                strcat(opt_content, "\n");
        }

        // show
        mjr_overlay(mjFONT_NORMAL, mjGRID_TOPRIGHT, rect, opt_title, opt_content, &con);
    }

    // showsensor data
    if( showsensor && m->nsensordata )
    {
        UpdateSensor();
        ShowSensor(rect);
    }

    // render bar
    rect.bottom = 0;
    rect.height = b.bar;
    mjr_rectangle(rect, .5, .5, .5, 1);
    char info[100];
    sprintf(info, "%lld / %lld", frame, numrec);
    mjr_overlay(mjFONT_NORMAL, mjGRID_BOTTOMLEFT, rect, info, NULL, &con);
    mjr_overlay(mjFONT_NORMAL, mjGRID_BOTTOMRIGHT, rect, paused ? "PAUSED" : "PLAYING", NULL, &con);
    mjrRect rline = {rect.width/4, b.bar/2-b.lwidth/2, rect.width/2, b.lwidth};
    mjr_rectangle(rline, .2, .2, .2, 1);
    mjrRect rcursor = {rect.width/4 + b.cpos-b.cwidth/2, b.bar/2-b.cheight/2, b.cwidth, b.cheight};
    mjr_rectangle(rcursor, 1, 1, 1, .8);

    // swap buffers
    glfwSwapBuffers(window);
}


int main(int argc, const char** argv)
{
    /// check arguments
    if( argc!=1 && argc!=2 )
    {
        printf("USAGE: playlog [fontscale]\n");
        return 1;
    }
    /// parse fontscale
    if( argc==2 )
    {
        sscanf(argv[2], "%lf", &fontscale);
        if( fontscale<1.25 )
            fontscale = 1;
        else if( fontscale>1.75 )
            fontscale = 2;
        else
            fontscale = 1.5;
    }
    /// parse file names
    paths = YAML::LoadFile(workspaceDir+"/src/cito/config/path.yaml");
    std::string modelFileTemp = paths["modelFile"].as<std::string>();
    const char *modelFile = modelFileTemp.c_str();
    std::string logFileTemp = paths["logFile"].as<std::string>();
    const char *logFile = logFileTemp.c_str();
    /// init log file
    std::ofstream outFile;
    outFile.open(paths["trajFile"].as<std::string>());
    /// write header to outFile
    if( outFile.is_open() )
    {
        outFile << "Total DOF: " << NV << ", actuated DOF: " << NU << "\n";
        outFile << "time,positions,velocities,accelerations,controls\n";
        outFile.close();
    }
    else std::cout << "Unable to open the trajectory file." << '\n';
    /// initialize OpenGL and MuJoCo
    initOpenGL(modelFile, logFile);
    initMuJoCo(modelFile, logFile);
    /// set GLFW callbacks
    glfwSetKeyCallback(window, keyboard);
    glfwSetCursorPosCallback(window, mouse_move);
    glfwSetMouseButtonCallback(window, mouse_button);
    glfwSetScrollCallback(window, scroll);
    glfwSetWindowRefreshCallback(window, render);
    InitSensor();
    /// main loop
    while( !glfwWindowShouldClose(window) )
    {
        // simulate and render
        render(window);
        // handle events (this calls all callbacks)
        glfwPollEvents();
    }
    /// free and terminate
    closeMuJoCo();
    /// terminate GLFW (crashes with Linux NVidia drivers)
    #if defined(__APPLE__) || defined(_WIN32)
        glfwTerminate();
    #endif
    return 0;
}
