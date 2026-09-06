#include <kernel.h>
#include <stdio.h>
#include "framedigest.h"
#include <GL/gl.h>   // The GL Header File
#include <GL/glut.h> // The GL Utility Toolkit (Glut) Header
#include <math.h>

float rtri;  // Angle For The Triangle
float rquad; // Angle For The Quad

//-----------------------------------------------------------------------------
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar)
{
    GLdouble xmin, xmax, ymin, ymax;

    ymax = zNear * tan(fovy * M_PI / 360.0f);
    ymin = -ymax;
    xmin = ymin * aspect;
    xmax = ymax * aspect;

    glFrustum(xmin, xmax, ymin, ymax, zNear, zFar);
}

void InitGL(GLvoid) // Create Some Everyday Functions
{

    glShadeModel(GL_SMOOTH);              // Enable Smooth Shading
    glClearColor(0.0f, 0.0f, 0.0f, 0.5f); // Black Background
    glClearDepth(1.0f);                   // Depth Buffer Setup
    glEnable(GL_DEPTH_TEST);              // Enables Depth Testing
    glDepthFunc(GL_LEQUAL);               // The Type Of Depth Testing To Do
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

    // ps2gl needs lighting + color_material for per-vertex colors
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
}

void idle(void) {
    
}

// The two frame buffers glut binds, as pages, and the format BITBLTBUF names
// them by.  Both are read every captured frame so which one the swap left in
// front does not have to be worked out.
static const unsigned framePages[] = {0, 70};
static const unsigned frameFormat = 1;
static const int frameWidth = 640;

// A window over the middle of the buffer, where the two shapes are, rather than
// the whole of it: the bus is turned around for the length of the read.
static const int windowX = 128;
static const int windowY = 32;
static const int windowWidth = 384;
static const int windowHeight = 160;
static const int windowStep = 12;

static const int capturedFrames = 8;
static int frameNumber = 0;

static void capture()
{
    printf("frame %d\n", frameNumber);
    for (unsigned i = 0; i < 2; ++i) {
        const FrameDigest::Result got =
            FrameDigest::digest(framePages[i], frameFormat, frameWidth, windowX,
                                windowY, windowWidth, windowHeight);
        printf("  page %3u: %d of %d quadwords, crc %08x\n", framePages[i],
               got.quadwordsRead, got.quadwordsWanted, got.crc);
        FrameDigest::printWindow(windowWidth, windowHeight, windowStep);
    }
}

void display(void)                                      // Create The Display Function
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // Clear Screen And Depth Buffer
    glLoadIdentity();                                   // Reset The Current Modelview Matrix
    glPushMatrix();
    glTranslatef(-1.5f, 0.0f, -6.0f);                   // Move Left 1.5 Units And Into The Screen 6.0
    glRotatef(rtri, 0.0f, 1.0f, 0.0f);                  // Rotate The Triangle On The Y axis
    glBegin(GL_TRIANGLES);                              // Drawing Using Triangles
    glColor3f(1.0f, 0.0f, 0.0f);                        // Red
    glVertex3f(0.0f, 1.0f, 0.0f);                       // Top Of Triangle (Front)
    glColor3f(0.0f, 1.0f, 0.0f);                        // Green
    glVertex3f(-1.0f, -1.0f, 1.0f);                     // Left Of Triangle (Front)
    glColor3f(0.0f, 0.0f, 1.0f);                        // Blue
    glVertex3f(1.0f, -1.0f, 1.0f);                      // Right Of Triangle (Front)
    glColor3f(1.0f, 0.0f, 0.0f);                        // Red
    glVertex3f(0.0f, 1.0f, 0.0f);                       // Top Of Triangle (Right)
    glColor3f(0.0f, 0.0f, 1.0f);                        // Blue
    glVertex3f(1.0f, -1.0f, 1.0f);                      // Left Of Triangle (Right)
    glColor3f(0.0f, 1.0f, 0.0f);                        // Green
    glVertex3f(1.0f, -1.0f, -1.0f);                     // Right Of Triangle (Right)
    glColor3f(1.0f, 0.0f, 0.0f);                        // Red
    glVertex3f(0.0f, 1.0f, 0.0f);                       // Top Of Triangle (Back)
    glColor3f(0.0f, 1.0f, 0.0f);                        // Green
    glVertex3f(1.0f, -1.0f, -1.0f);                     // Left Of Triangle (Back)
    glColor3f(0.0f, 0.0f, 1.0f);                        // Blue
    glVertex3f(-1.0f, -1.0f, -1.0f);                    // Right Of Triangle (Back)
    glColor3f(1.0f, 0.0f, 0.0f);                        // Red
    glVertex3f(0.0f, 1.0f, 0.0f);                       // Top Of Triangle (Left)
    glColor3f(0.0f, 0.0f, 1.0f);                        // Blue
    glVertex3f(-1.0f, -1.0f, -1.0f);                    // Left Of Triangle (Left)
    glColor3f(0.0f, 1.0f, 0.0f);                        // Green
    glVertex3f(-1.0f, -1.0f, 1.0f);                     // Right Of Triangle (Left)
    glEnd();                                            // Finished Drawing The Triangle

    glLoadIdentity();                                   // Reset The Current Modelview Matrix
    glTranslatef(1.5f, 0.0f, -6.0f);                    // Move Right 1.5 Units And Into The Screen 6.0
    glRotatef(rquad, 1.0f, 0.0f, 0.0f);                 // Rotate The Quad On The X axis
    glColor3f(0.5f, 0.5f, 1.0f);                        // Set The Color To Blue One Time Only
    glBegin(GL_QUADS);                                  // Draw A Quad
    glColor3f(0.0f, 1.0f, 0.0f);                        // Set The Color To Blue
    glVertex3f(1.0f, 1.0f, -1.0f);                      // Top Right Of The Quad (Top)
    glVertex3f(-1.0f, 1.0f, -1.0f);                     // Top Left Of The Quad (Top)
    glVertex3f(-1.0f, 1.0f, 1.0f);                      // Bottom Left Of The Quad (Top)
    glVertex3f(1.0f, 1.0f, 1.0f);                       // Bottom Right Of The Quad (Top)
    glColor3f(1.0f, 0.5f, 0.0f);                        // Set The Color To Orange
    glVertex3f(1.0f, -1.0f, 1.0f);                      // Top Right Of The Quad (Bottom)
    glVertex3f(-1.0f, -1.0f, 1.0f);                     // Top Left Of The Quad (Bottom)
    glVertex3f(-1.0f, -1.0f, -1.0f);                    // Bottom Left Of The Quad (Bottom)
    glVertex3f(1.0f, -1.0f, -1.0f);                     // Bottom Right Of The Quad (Bottom)
    glColor3f(1.0f, 0.0f, 0.0f);                        // Set The Color To Red
    glVertex3f(1.0f, 1.0f, 1.0f);                       // Top Right Of The Quad (Front)
    glVertex3f(-1.0f, 1.0f, 1.0f);                      // Top Left Of The Quad (Front)
    glVertex3f(-1.0f, -1.0f, 1.0f);                     // Bottom Left Of The Quad (Front)
    glVertex3f(1.0f, -1.0f, 1.0f);                      // Bottom Right Of The Quad (Front)
    glColor3f(1.0f, 1.0f, 0.0f);                        // Set The Color To Yellow
    glVertex3f(1.0f, -1.0f, -1.0f);                     // Bottom Left Of The Quad (Back)
    glVertex3f(-1.0f, -1.0f, -1.0f);                    // Bottom Right Of The Quad (Back)
    glVertex3f(-1.0f, 1.0f, -1.0f);                     // Top Right Of The Quad (Back)
    glVertex3f(1.0f, 1.0f, -1.0f);                      // Top Left Of The Quad (Back)
    glColor3f(0.0f, 0.0f, 1.0f);                        // Set The Color To Blue
    glVertex3f(-1.0f, 1.0f, 1.0f);                      // Top Right Of The Quad (Left)
    glVertex3f(-1.0f, 1.0f, -1.0f);                     // Top Left Of The Quad (Left)
    glVertex3f(-1.0f, -1.0f, -1.0f);                    // Bottom Left Of The Quad (Left)
    glVertex3f(-1.0f, -1.0f, 1.0f);                     // Bottom Right Of The Quad (Left)
    glColor3f(1.0f, 0.0f, 1.0f);                        // Set The Color To Violet
    glVertex3f(1.0f, 1.0f, -1.0f);                      // Top Right Of The Quad (Right)
    glVertex3f(1.0f, 1.0f, 1.0f);                       // Top Left Of The Quad (Right)
    glVertex3f(1.0f, -1.0f, 1.0f);                      // Bottom Left Of The Quad (Right)
    glVertex3f(1.0f, -1.0f, -1.0f);                     // Bottom Right Of The Quad (Right)
    glEnd();                                            // Done Drawing The Quad

    glPopMatrix();
    rtri += 0.2f;   // Increase The Rotation Variable For The Triangle ( NEW )
    rquad -= 0.15f; // Decrease The Rotation Variable For The Quad     ( NEW )


    glutSwapBuffers();
    // Swap The Buffers To Not Be Left With A Clear Screen

    capture();
    frameNumber++;
    if (frameNumber >= capturedFrames) {
        printf("-- TEST END\n");
        SleepThread();
    }
}

void reshape(int width, int height) // Create The Reshape Function (the viewport)
{
    if (height == 0)                // Prevent A Divide By Zero By
    {
        height = 1;                 // Making Height Equal One
    }

    glViewport(0, 0, width, height); // Reset The Current Viewport

    glMatrixMode(GL_PROJECTION);     // Select The Projection Matrix
    glLoadIdentity();                // Reset The Projection Matrix

    // Calculate The Aspect Ratio Of The Window
    gluPerspective(45.0f, (GLfloat)width / (GLfloat)height, 0.1f, 100.0f);

    glMatrixMode(GL_MODELVIEW); // Select The Modelview Matrix
    glLoadIdentity();
}

int main(int argc, char **argv)                   // Create Main Function For Bringing It All Together
{
    printf("-- TEST BEGIN\n");
    glutInit(&argc, argv);                        // Erm Just Write It =)
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE); // Display Mode
    glutInitWindowSize(640, 448);                 // If glutFullScreen wasn't called this is the window size
    glutCreateWindow("NeHe's OpenGL Framework");  // Window Title (argv[0] for current directory as title)
    InitGL();
    glutDisplayFunc(display);                     // Matching Earlier Functions To Their Counterparts
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);
    glutMainLoop(); // Initialize The Main Loop

    return 0;
}
