/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2013 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include "SimpleSkyNode"
#include "SimpleSkyShaders"

#include <osgEarthUtil/StarData>

#include <osgEarth/VirtualProgram>
#include <osgEarth/NodeUtils>
#include <osgEarth/Map>
#include <osgEarth/Utils>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/CullingUtils>

#include <osg/MatrixTransform>
#include <osg/ShapeDrawable>
#include <osg/PointSprite>
#include <osg/BlendFunc>
#include <osg/FrontFace>
#include <osg/CullFace>
#include <osg/Program>
#include <osg/Camera>
#include <osg/Point>
#include <osg/Shape>
#include <osg/Depth>
#include <osg/Quat>

#include <sstream>
#include <time.h>

#define LC "[SimpleSkyNode] "

using namespace osgEarth;
using namespace osgEarth::Util;
using namespace osgEarth::Drivers::SimpleSky;

//---------------------------------------------------------------------------

#define BIN_STARS       -100003
#define BIN_SUN         -100002
#define BIN_MOON        -100001
#define BIN_ATMOSPHERE  -100000

#define TWO_PI 6.283185307179586476925286766559

//---------------------------------------------------------------------------

namespace
{
    // constucts an ellipsoidal mesh that we will use to draw the atmosphere
    osg::Geometry* s_makeEllipsoidGeometry(const osg::EllipsoidModel* ellipsoid, 
                                           double                     outerRadius, 
                                           bool                       genTexCoords)
    {
        double hae = outerRadius - ellipsoid->getRadiusEquator();

        osg::Geometry* geom = new osg::Geometry();
        geom->setUseVertexBufferObjects(true);

        int latSegments = 100;
        int lonSegments = 2 * latSegments;

        double segmentSize = 180.0/(double)latSegments; // degrees

        osg::Vec3Array* verts = new osg::Vec3Array();
        verts->reserve( latSegments * lonSegments );

        osg::Vec2Array* texCoords = 0;
        osg::Vec3Array* normals = 0;
        if (genTexCoords)
        {
            texCoords = new osg::Vec2Array();
            texCoords->reserve( latSegments * lonSegments );
            geom->setTexCoordArray( 0, texCoords );

            normals = new osg::Vec3Array();
            normals->reserve( latSegments * lonSegments );
            geom->setNormalArray( normals );
            geom->setNormalBinding(osg::Geometry::BIND_PER_VERTEX );
        }

        osg::DrawElementsUShort* el = new osg::DrawElementsUShort( GL_TRIANGLES );
        el->reserve( latSegments * lonSegments * 6 );

        for( int y = 0; y <= latSegments; ++y )
        {
            double lat = -90.0 + segmentSize * (double)y;
            for( int x = 0; x < lonSegments; ++x )
            {
                double lon = -180.0 + segmentSize * (double)x;
                double gx, gy, gz;
                ellipsoid->convertLatLongHeightToXYZ( osg::DegreesToRadians(lat), osg::DegreesToRadians(lon), hae, gx, gy, gz );
                verts->push_back( osg::Vec3(gx, gy, gz) );

                if (genTexCoords)
                {
                    double s = (lon + 180) / 360.0;
                    double t = (lat + 90.0) / 180.0;
                    texCoords->push_back( osg::Vec2(s, t ) );
                }

                if (normals)
                {
                    osg::Vec3 normal( gx, gy, gz);
                    normal.normalize();
                    normals->push_back( normal );
                }


                if ( y < latSegments )
                {
                    int x_plus_1 = x < lonSegments-1 ? x+1 : 0;
                    int y_plus_1 = y+1;
                    el->push_back( y*lonSegments + x );
                    el->push_back( y_plus_1*lonSegments + x );
                    el->push_back( y*lonSegments + x_plus_1 );
                    el->push_back( y*lonSegments + x_plus_1 );
                    el->push_back( y_plus_1*lonSegments + x );
                    el->push_back( y_plus_1*lonSegments + x_plus_1 );
                }
            }
        }

        geom->setVertexArray( verts );
        geom->addPrimitiveSet( el );

        return geom;
    }

    // makes a disc geometry that we'll use to render the sun/moon
    osg::Geometry* s_makeDiscGeometry(double radius)
    {
        int segments = 48;
        float deltaAngle = 360.0/(float)segments;

        osg::Geometry* geom = new osg::Geometry();
        geom->setUseVertexBufferObjects(true);

        osg::Vec3Array* verts = new osg::Vec3Array();
        verts->reserve( 1 + segments );
        geom->setVertexArray( verts );

        osg::DrawElementsUShort* el = new osg::DrawElementsUShort( GL_TRIANGLES );
        el->reserve( 1 + 2*segments );
        geom->addPrimitiveSet( el );

        verts->push_back( osg::Vec3(0,0,0) ); // center point

        for( int i=0; i<segments; ++i )
        {
            double angle = osg::DegreesToRadians( deltaAngle * (float)i );
            double x = radius * cos( angle );
            double y = radius * sin( angle );
            verts->push_back( osg::Vec3(x, y, 0.0) );

            int i_plus_1 = i < segments-1? i+1 : 0;
            el->push_back( 0 );
            el->push_back( 1 + i_plus_1 );
            el->push_back( 1 + i );
        }

        return geom;
    }
}

#if 0
//---------------------------------------------------------------------------

namespace
{
    // Atmospheric Scattering and Sun Shaders
    // Adapted from code that is Copyright (c) 2004 Sean O'Neil

    static char s_versionString[] =
        "#version " GLSL_VERSION_STR "\n";

    static char s_mathUtils[] =
        "float fastpow( in float x, in float y ) \n"
        "{ \n"
        "    return x/(x+y-y*x); \n"
        "} \n";

    static char s_atmosphereVertexDeclarations[] =
        "uniform mat4 osg_ViewMatrixInverse;     // camera position \n"
        "uniform vec3 atmos_v3LightPos;        // The direction vector to the light source \n"
        "uniform vec3 atmos_v3InvWavelength;   // 1 / pow(wavelength,4) for the rgb channels \n"
        "uniform float atmos_fOuterRadius;     // Outer atmosphere radius \n"
        "uniform float atmos_fOuterRadius2;    // fOuterRadius^2 \n"		
        "uniform float atmos_fInnerRadius;     // Inner planetary radius \n"
        "uniform float atmos_fInnerRadius2;    // fInnerRadius^2 \n"
        "uniform float atmos_fKrESun;          // Kr * ESun \n"	
        "uniform float atmos_fKmESun;          // Km * ESun \n"		
        "uniform float atmos_fKr4PI;           // Kr * 4 * PI \n"	
        "uniform float atmos_fKm4PI;           // Km * 4 * PI \n"		
        "uniform float atmos_fScale;           // 1 / (fOuterRadius - fInnerRadius) \n"	
        "uniform float atmos_fScaleDepth;      // The scale depth \n"
        "uniform float atmos_fScaleOverScaleDepth;     // fScale / fScaleDepth \n"	
        "uniform int atmos_nSamples; \n"	
        "uniform float atmos_fSamples; \n"				

        "varying vec3 atmos_v3Direction; \n"
        "varying vec3 atmos_mieColor; \n"
        "varying vec3 atmos_rayleighColor; \n"

        "vec3 vVec; \n"
        "float atmos_fCameraHeight;    // The camera's current height \n"		
        "float atmos_fCameraHeight2;   // fCameraHeight^2 \n";

    static char s_atmosphereVertexShared[] =
        "float atmos_scale(float fCos) \n"	
        "{ \n"
        "    float x = 1.0 - fCos; \n"
        "    return atmos_fScaleDepth * exp(-0.00287 + x*(0.459 + x*(3.83 + x*(-6.80 + x*5.25)))); \n"
        "} \n"

        "void SkyFromSpace(void) \n"
        "{ \n"
        "    // Get the ray from the camera to the vertex and its length (which is the far point of the ray passing through the atmosphere) \n"
        "    vec3 v3Pos = gl_Vertex.xyz; \n"
        "    vec3 v3Ray = v3Pos - vVec; \n"
        "    float fFar = length(v3Ray); \n"
        "    v3Ray /= fFar; \n"

        "    // Calculate the closest intersection of the ray with the outer atmosphere \n"
        "    // (which is the near point of the ray passing through the atmosphere) \n"
        "    float B = 2.0 * dot(vVec, v3Ray); \n"
        "    float C = atmos_fCameraHeight2 - atmos_fOuterRadius2; \n"
        "    float fDet = max(0.0, B*B - 4.0 * C); \n"	
        "    float fNear = 0.5 * (-B - sqrt(fDet)); \n"		

        "    // Calculate the ray's starting position, then calculate its atmos_ing offset \n"
        "    vec3 v3Start = vVec + v3Ray * fNear; \n"			
        "    fFar -= fNear; \n"	
        "    float fStartAngle = dot(v3Ray, v3Start) / atmos_fOuterRadius; \n"			
        "    float fStartDepth = exp(-1.0 / atmos_fScaleDepth); \n"
        "    float fStartOffset = fStartDepth*atmos_scale(fStartAngle); \n"		

        "    // Initialize the atmos_ing loop variables \n"	
        "    float fSampleLength = fFar / atmos_fSamples; \n"		
        "    float fScaledLength = fSampleLength * atmos_fScale; \n"					
        "    vec3 v3SampleRay = v3Ray * fSampleLength; \n"	
        "    vec3 v3SamplePoint = v3Start + v3SampleRay * 0.5; \n"	

        "    // Now loop through the sample rays \n"
        "    vec3 v3FrontColor = vec3(0.0, 0.0, 0.0); \n"
        "    vec3 v3Attenuate; \n"  
        "    for(int i=0; i<atmos_nSamples; i++) \n"		
        "    { \n"
        "        float fHeight = length(v3SamplePoint); \n"			
        "        float fDepth = exp(atmos_fScaleOverScaleDepth * (atmos_fInnerRadius - fHeight)); \n"
        "        float fLightAngle = dot(atmos_v3LightPos, v3SamplePoint) / fHeight; \n"		
        "        float fCameraAngle = dot(v3Ray, v3SamplePoint) / fHeight; \n"			
        "        float fscatter = (fStartOffset + fDepth*(atmos_scale(fLightAngle) - atmos_scale(fCameraAngle))); \n"	
        "        v3Attenuate = exp(-fscatter * (atmos_v3InvWavelength * atmos_fKr4PI + atmos_fKm4PI)); \n"	
        "        v3FrontColor += v3Attenuate * (fDepth * fScaledLength); \n"					
        "        v3SamplePoint += v3SampleRay; \n"		
        "    } \n"		

        "    // Finally, scale the Mie and Rayleigh colors and set up the varying \n"			
        "    // variables for the pixel shader \n"	
        "    atmos_mieColor      = v3FrontColor * atmos_fKmESun; \n"				
        "    atmos_rayleighColor = v3FrontColor * (atmos_v3InvWavelength * atmos_fKrESun); \n"						
        "    atmos_v3Direction = vVec  - v3Pos; \n"			
        "} \n"		

        "void SkyFromAtmosphere(void) \n"		
        "{ \n"
        "  // Get the ray from the camera to the vertex, and its length (which is the far \n"
        "  // point of the ray passing through the atmosphere) \n"		
        "  vec3 v3Pos = gl_Vertex.xyz; \n"	
        "  vec3 v3Ray = v3Pos - vVec; \n"			
        "  float fFar = length(v3Ray); \n"					
        "  v3Ray /= fFar; \n"				

        "  // Calculate the ray's starting position, then calculate its atmos_ing offset \n"
        "  vec3 v3Start = vVec; \n"
        "  float fHeight = length(v3Start); \n"		
        "  float fDepth = exp(atmos_fScaleOverScaleDepth * (atmos_fInnerRadius - atmos_fCameraHeight)); \n"
        "  float fStartAngle = dot(v3Ray, v3Start) / fHeight; \n"	
        "  float fStartOffset = fDepth*atmos_scale(fStartAngle); \n"

        "  // Initialize the atmos_ing loop variables \n"		
        "  float fSampleLength = fFar / atmos_fSamples; \n"			
        "  float fScaledLength = fSampleLength * atmos_fScale; \n"				
        "  vec3 v3SampleRay = v3Ray * fSampleLength; \n"		
        "  vec3 v3SamplePoint = v3Start + v3SampleRay * 0.5; \n"

        "  // Now loop through the sample rays \n"		
        "  vec3 v3FrontColor = vec3(0.0, 0.0, 0.0); \n"		
        "  vec3 v3Attenuate; \n"  
        "  for(int i=0; i<atmos_nSamples; i++) \n"			
        "  { \n"	
        "    float fHeight = length(v3SamplePoint); \n"	
        "    float fDepth = exp(atmos_fScaleOverScaleDepth * (atmos_fInnerRadius - fHeight)); \n"
        "    float fLightAngle = dot(atmos_v3LightPos, v3SamplePoint) / fHeight; \n"
        "    float fCameraAngle = dot(v3Ray, v3SamplePoint) / fHeight; \n"	
        "    float fscatter = (fStartOffset + fDepth*(atmos_scale(fLightAngle) - atmos_scale(fCameraAngle))); \n"	
        "    v3Attenuate = exp(-fscatter * (atmos_v3InvWavelength * atmos_fKr4PI + atmos_fKm4PI)); \n"	
        "    v3FrontColor += v3Attenuate * (fDepth * fScaledLength); \n"		
        "    v3SamplePoint += v3SampleRay; \n"		
        "  } \n"

        "  // Finally, scale the Mie and Rayleigh colors and set up the varying \n"
        "  // variables for the pixel shader \n"					
        "  atmos_mieColor      = v3FrontColor * atmos_fKmESun; \n"			
        "  atmos_rayleighColor = v3FrontColor * (atmos_v3InvWavelength * atmos_fKrESun); \n"				
        "  atmos_v3Direction = vVec - v3Pos; \n"				
        "} \n";


    static char s_atmosphereVertexMain[] =
        "void main(void) \n"
        "{ \n"
        "  // Get camera position and height \n"
        "  vVec = osg_ViewMatrixInverse[3].xyz; \n"
        "  atmos_fCameraHeight = length(vVec); \n"
        "  atmos_fCameraHeight2 = atmos_fCameraHeight*atmos_fCameraHeight; \n"
        "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; \n"
        "  if(atmos_fCameraHeight >= atmos_fOuterRadius) { \n"
        "      SkyFromSpace(); \n"
        "  } \n"
        "  else { \n"
        "      SkyFromAtmosphere(); \n"
        "  } \n"
        "} \n";

    static char s_atmosphereFragmentDeclarations[] =
        "uniform vec3 atmos_v3LightPos; \n"							
        "uniform float atmos_g; \n"				
        "uniform float atmos_g2; \n"
        "uniform float atmos_fWeather; \n"

        "varying vec3 atmos_v3Direction; \n"	
        "varying vec3 atmos_mieColor; \n"
        "varying vec3 atmos_rayleighColor; \n"

        "const float fExposure = 4.0; \n";

    static char s_atmosphereFragmentMain[] =
        "void main(void) \n"			
        "{ \n"				
        "    float fCos = dot(atmos_v3LightPos, atmos_v3Direction) / length(atmos_v3Direction); \n"
        "    float fRayleighPhase = 1.0; \n" // 0.75 * (1.0 + fCos*fCos); \n"
        "    float fMiePhase = 1.5 * ((1.0 - atmos_g2) / (2.0 + atmos_g2)) * (1.0 + fCos*fCos) / fastpow(1.0 + atmos_g2 - 2.0*atmos_g*fCos, 1.5); \n"
        "    vec3 f4Color = fRayleighPhase * atmos_rayleighColor + fMiePhase * atmos_mieColor; \n"
        "    vec3 color = 1.0 - exp(f4Color * -fExposure); \n"
        "    gl_FragColor.rgb = color.rgb*atmos_fWeather; \n"
        "    gl_FragColor.a = (color.r+color.g+color.b) * 2.0; \n"
        "} \n";

    static char s_sunVertexSource[] = 
        "varying vec3 atmos_v3Direction; \n"

        "void main() \n"
        "{ \n"
        "    vec3 v3Pos = gl_Vertex.xyz; \n"
        "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; \n"
        "    atmos_v3Direction = vec3(0.0,0.0,1.0) - v3Pos; \n"
        "    atmos_v3Direction = atmos_v3Direction/length(atmos_v3Direction); \n"
        "} \n";

    static char s_sunFragmentSource[] =
        "uniform float sunAlpha; \n"
        "varying vec3 atmos_v3Direction; \n"

        "void main( void ) \n"
        "{ \n"
        "   float fCos = -atmos_v3Direction[2]; \n"         
        "   float fMiePhase = 0.050387596899224826 * (1.0 + fCos*fCos) / fastpow(1.9024999999999999 - -1.8999999999999999*fCos, 1.5); \n"
        "   gl_FragColor.rgb = fMiePhase*vec3(.3,.3,.2); \n"
        "   gl_FragColor.a = sunAlpha*gl_FragColor.r; \n"
        "} \n";

    static char s_moonVertexSource[] = 
        "uniform mat4 osg_ModelViewProjectionMatrix;"
        "varying vec4 moon_TexCoord;\n"
        "void main() \n"
        "{ \n"
        "    moon_TexCoord = gl_MultiTexCoord0; \n"
        "    gl_Position = osg_ModelViewProjectionMatrix * gl_Vertex; \n"
        "} \n";

    static char s_moonFragmentSource[] =
        "varying vec4 moon_TexCoord;\n"
        "uniform sampler2D moonTex;\n"
        "void main( void ) \n"
        "{ \n"
        "   gl_FragColor = texture2D(moonTex, moon_TexCoord.st);\n"
        "} \n";
}

//---------------------------------------------------------------------------

namespace
{
    static std::string s_createStarVertexSource()
    {
        float glslVersion = Registry::instance()->getCapabilities().getGLSLVersion();

        return Stringify()
            << "#version " << (glslVersion < 1.2f ? GLSL_VERSION_STR : "120") << "\n"

            << "float remap( float val, float vmin, float vmax, float r0, float r1 ) \n"
            << "{ \n"
            << "    float vr = (clamp(val, vmin, vmax)-vmin)/(vmax-vmin); \n"
            << "    return r0 + vr * (r1-r0); \n"
            << "} \n"

            << "uniform vec3 atmos_v3LightPos; \n"
            << "uniform mat4 osg_ViewMatrixInverse; \n"
            << "varying float visibility; \n"
            << "varying vec4 osg_FrontColor; \n"
            << "void main() \n"
            << "{ \n"
            << "    osg_FrontColor = gl_Color; \n"
            << "    gl_PointSize = gl_Color.r * " << (glslVersion < 1.2f ? "2.0" : "14.0") << ";\n"
            << "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; \n"

            << "    vec3 eye = osg_ViewMatrixInverse[3].xyz; \n"
            << "    float hae = length(eye) - 6378137.0; \n"
            // "highness": visibility increases with altitude
            << "    float highness = remap( hae, 25000.0, 150000.0, 0.0, 1.0 ); \n"
            << "    eye = normalize(eye); \n"
            // "darkness": visibility increase as the sun goes around the other side of the earth
            << "    float darkness = 1.0-remap(dot(eye,atmos_v3LightPos), -0.25, 0.0, 0.0, 1.0); \n"
            << "    visibility = clamp(highness + darkness, 0.0, 1.0); \n"
            << "} \n";
    }

    static std::string s_createStarFragmentSource()
    {
        float glslVersion = Registry::instance()->getCapabilities().getGLSLVersion();

        if ( glslVersion < 1.2f )
        {
            return Stringify()
                << "#version " << GLSL_VERSION_STR << "\n"
#ifdef OSG_GLES2_AVAILABLE
                << "precision highp float;\n"
#endif  
                << "varying float visibility; \n"
                << "varying vec4 osg_FrontColor; \n"
                << "void main( void ) \n"
                << "{ \n"
                << "    gl_FragColor = osg_FrontColor * visibility; \n"
                << "} \n";
        }
        else
        {
            return Stringify()
                << "#version 120 \n"
#ifdef OSG_GLES2_AVAILABLE
                << "precision highp float;\n"
#endif
                << "varying float visibility; \n"
                << "varying vec4 osg_FrontColor; \n"
                << "void main( void ) \n"
                << "{ \n"
                << "    float b1 = 1.0-(2.0*abs(gl_PointCoord.s-0.5)); \n"
                << "    float b2 = 1.0-(2.0*abs(gl_PointCoord.t-0.5)); \n"
                << "    float i = b1*b1 * b2*b2; \n"
                << "    gl_FragColor = osg_FrontColor * i * visibility; \n"
                << "} \n";
        }
    }
}
#endif

//---------------------------------------------------------------------------

SimpleSkyNode::SimpleSkyNode(const Map* map) :
SkyNode()
{
    initialize(map);
}

SimpleSkyNode::SimpleSkyNode(const Map*              map,
                             const SimpleSkyOptions& options) :
SkyNode ( options ),
_options( options )
{
    initialize(map);
}

void
SimpleSkyNode::initialize(const Map* map)
{
    // intialize the default settings:
    _defaultPerViewData._lightPos.set( osg::Vec3f(0.0f, 1.0f, 0.0f) );
    _defaultPerViewData._light = new osg::Light( 0 );  
    _defaultPerViewData._light->setPosition( osg::Vec4( _defaultPerViewData._lightPos, 0 ) );
    _defaultPerViewData._light->setAmbient( osg::Vec4(0.2f, 0.2f, 0.2f, 2.0) );
    _defaultPerViewData._light->setDiffuse( osg::Vec4(1,1,1,1) );
    _defaultPerViewData._light->setSpecular( osg::Vec4(0,0,0,1) );

    // set up the uniform that conveys the normalized light position in world space
    _defaultPerViewData._lightPosUniform = new osg::Uniform( osg::Uniform::FLOAT_VEC3, "atmos_v3LightPos" );
    _defaultPerViewData._lightPosUniform->set( _defaultPerViewData._lightPos / _defaultPerViewData._lightPos.length() );

    // set up the astronomical parameters:
    _ellipsoidModel = map->getProfile()->getSRS()->getGeographicSRS()->getEllipsoid();
    _innerRadius = _ellipsoidModel->getRadiusPolar();
    _outerRadius = _innerRadius * 1.025f;
    _sunDistance = _innerRadius * 12000.0f;

    // make the uniforms and the terrain lighting shaders.
    makeLighting();

    // make the sky elements (don't change the order here)
    makeAtmosphere( _ellipsoidModel.get() );

    makeSun();

    makeMoon();

    if (_minStarMagnitude < 0)
    {
        const char* magEnv = ::getenv("OSGEARTH_MIN_STAR_MAGNITUDE");
        if (magEnv)
            _minStarMagnitude = as<float>(std::string(magEnv), -1.0f);
    }

    makeStars();

    // automatically compute ambient lighting based on the eyepoint
    _autoAmbience = false;

    // Update everything based on the date/time.
    onSetDateTime();
}

osg::BoundingSphere
SimpleSkyNode::computeBound() const
{
    return osg::BoundingSphere();
}

void
SimpleSkyNode::traverse( osg::NodeVisitor& nv )
{    
    osgUtil::CullVisitor* cv = Culling::asCullVisitor(nv);
    if ( cv )
    {

        // If there's a custom projection matrix clamper installed, remove it temporarily.
        // We dont' want it mucking with our sky elements.
        osg::ref_ptr<osg::CullSettings::ClampProjectionMatrixCallback> cb = cv->getClampProjectionMatrixCallback();
        cv->setClampProjectionMatrixCallback( 0L );

        osg::View* view = cv->getCurrentCamera()->getView();


        //Try to find the per view data for camera's view if there is one.
        PerViewDataMap::iterator itr = _perViewData.find( view );

        if ( itr == _perViewData.end() )
        {
            // If we don't find any per view data, just use the first one that is stored.
            // This needs to be reworked to be per camera and also to automatically create a 
            // new data structure on demand since camera's can be added/removed on the fly.
            itr = _perViewData.begin();
        }


        if ( _autoAmbience )
        {
            const float minAmb = 0.2f;
            const float maxAmb = 0.92f;
            const float minDev = -0.2f;
            const float maxDev = 0.75f;
            osg::Vec3 eye = cv->getViewPoint(); eye.normalize();
            osg::Vec3 sun = itr->second._lightPos; sun.normalize();
            float dev = osg::clampBetween(eye*sun, minDev, maxDev);
            float r   = (dev-minDev)/(maxDev-minDev);
            float amb = minAmb + r*(maxAmb-minAmb);
            itr->second._light->setAmbient( osg::Vec4(amb,amb,amb,1.0) );
            //OE_INFO << "dev=" << dev << ", amb=" << amb << std::endl;
        }

        itr->second._cullContainer->accept( nv );

        // restore a custom clamper.
        if ( cb.valid() ) cv->setClampProjectionMatrixCallback( cb.get() );
    }

    else
    {
        osg::Group::traverse( nv );
    }
}

void
SimpleSkyNode::onSetEphemeris()
{
    // trigger the date/time update.
    onSetDateTime();
}

void
SimpleSkyNode::onSetDateTime()
{
    if ( _ellipsoidModel.valid() )
    {
        osg::View* view = 0L;
        const DateTime& dt = getDateTime();

        osg::Vec3d sunPos = getEphemeris()->getSunPositionECEF( dt );
        osg::Vec3d moonPos = getEphemeris()->getMoonPositionECEF( dt );

        sunPos.normalize();
        setSunPosition( sunPos, view );
        setMoonPosition( moonPos, view );

        // position the stars:
        double time_r = dt.hours()/24.0; // 0..1
        double rot_z = -osg::PI + TWO_PI*time_r;

        osg::Matrixd starsMatrix = osg::Matrixd::rotate( -rot_z, 0, 0, 1 );
        if ( !view )
        {
            _defaultPerViewData._starsMatrix = starsMatrix;
            _defaultPerViewData._date = dt;

            for( PerViewDataMap::iterator i = _perViewData.begin(); i != _perViewData.end(); ++i )
            {
                i->second._starsMatrix = starsMatrix;
                i->second._starsXform->setMatrix( starsMatrix );
                i->second._date = dt;
            }
        }
        else if ( _perViewData.find(view) != _perViewData.end() )
        {
            PerViewData& data = _perViewData[view];
            data._starsMatrix = starsMatrix;
            data._starsXform->setMatrix( starsMatrix );
            data._date = dt;
        }
    }
}

void
SimpleSkyNode::attach( osg::View* view, int lightNum )
{
    if ( !view ) return;

    // creates the new per-view if it does not already exist
    PerViewData& data = _perViewData[view];

    data._light = osg::clone( _defaultPerViewData._light.get() );
    data._light->setLightNum( lightNum );
    data._light->setAmbient( _defaultPerViewData._light->getAmbient() );
    data._lightPos = _defaultPerViewData._lightPos;

    // the cull callback has to be on a parent group-- won't work on the xforms themselves.
    data._cullContainer = new osg::Group();

    data._sunXform = new osg::MatrixTransform();
    data._sunMatrix = osg::Matrixd::translate(
        _sunDistance * data._lightPos.x(),
        _sunDistance * data._lightPos.y(),
        _sunDistance * data._lightPos.z() );
    data._sunXform->setMatrix( data._sunMatrix );
    data._sunXform->addChild( _sun.get() );
    data._sunXform->setNodeMask( getSunVisible() ? ~0 : 0 );
    data._cullContainer->addChild( data._sunXform.get() );

    data._moonXform = new osg::MatrixTransform();
    data._moonMatrix = _defaultPerViewData._moonMatrix;
    data._moonXform->setMatrix( data._moonMatrix );
    data._moonXform->addChild( _moon.get() );
    data._moonXform->setNodeMask( getMoonVisible() ? ~0 : 0 );
    data._cullContainer->addChild( data._moonXform.get() );

    data._starsXform = new osg::MatrixTransform();
    data._starsMatrix = _defaultPerViewData._starsMatrix;
    data._starsXform->setMatrix( _defaultPerViewData._starsMatrix );
    data._starsXform->addChild( _stars.get() );
    data._starsXform->setNodeMask( getStarsVisible() ? ~0 : 0 );
    data._cullContainer->addChild( data._starsXform.get() );

    data._cullContainer->addChild( _atmosphere.get() );
    data._lightPosUniform = osg::clone( _defaultPerViewData._lightPosUniform.get() );
    data._cullContainer->getOrCreateStateSet()->addUniform( data._lightPosUniform.get() );

    // node to traverse the child nodes
    data._cullContainer->addChild( new TraverseNode<osg::Group>(this) );

    view->setLightingMode( osg::View::SKY_LIGHT );
    view->setLight( data._light.get() );
    view->getCamera()->setClearColor( osg::Vec4(0,0,0,1) );

    data._date = _defaultPerViewData._date;
}

#if 0
void
SimpleSkyNode::setAmbientBrightness( float value, osg::View* view )
{
    if ( !view )
    {
        setAmbientBrightness( _defaultPerViewData, value );

        for( PerViewDataMap::iterator i = _perViewData.begin(); i != _perViewData.end(); ++i )
            setAmbientBrightness( i->second, value );
    }
    else if ( _perViewData.find(view) != _perViewData.end() )
    {
        setAmbientBrightness( _perViewData[view], value );
    }
}

float
SimpleSkyNode::getAmbientBrightness( osg::View* view ) const
{
    if ( view )
    {
        PerViewDataMap::const_iterator i = _perViewData.find(view);
        if ( i != _perViewData.end() )
            return i->second._light->getAmbient().r();
    }
    return _defaultPerViewData._light->getAmbient().r();
}

void
SimpleSkyNode::setAutoAmbience( bool value )
{
    _autoAmbience = value;
}

bool
SimpleSkyNode::getAutoAmbience() const
{
    return _autoAmbience;
}

void 
SimpleSkyNode::setAmbientBrightness( PerViewData& data, float value )
{
    value = osg::clampBetween( value, 0.0f, 1.0f );
    data._light->setAmbient( osg::Vec4f(value, value, value, 1.0f) );
    _autoAmbience = false;
}
#endif

void
SimpleSkyNode::setSunPosition( const osg::Vec3& pos, osg::View* view )
{
    if ( !view )
    {
        setSunPosition( _defaultPerViewData, pos );
        for( PerViewDataMap::iterator i = _perViewData.begin(); i != _perViewData.end(); ++i )
            setSunPosition( i->second, pos );
    }
    else if ( _perViewData.find(view) != _perViewData.end() )
    {
        setSunPosition( _perViewData[view], pos );
    }
}

void
SimpleSkyNode::setMoonPosition( const osg::Vec3d& pos, osg::View* view )
{
    _moonPosition = pos;
    if ( !view )
    {
        setMoonPosition( _defaultPerViewData, pos );
        for( PerViewDataMap::iterator i = _perViewData.begin(); i != _perViewData.end(); ++i )
            setMoonPosition( i->second, pos );
    }
    else if ( _perViewData.find(view) != _perViewData.end() )
    {
        setMoonPosition( _perViewData[view], pos );
    }
}

void
SimpleSkyNode::setSunPosition( PerViewData& data, const osg::Vec3& pos )
{
    data._lightPos = pos;

    if ( data._light.valid() )
        data._light->setPosition( osg::Vec4( data._lightPos, 0 ) );

    if ( data._lightPosUniform.valid() )
        data._lightPosUniform->set( data._lightPos / data._lightPos.length() );

    if ( data._sunXform.valid() )
    {
        data._sunXform->setMatrix( osg::Matrix::translate( 
            _sunDistance * data._lightPos.x(), 
            _sunDistance * data._lightPos.y(),
            _sunDistance * data._lightPos.z() ) );
    }
}

void
SimpleSkyNode::setSunPosition( double lat_degrees, double long_degrees, osg::View* view )
{
    if (_ellipsoidModel.valid())
    {
        double x, y, z;
        _ellipsoidModel->convertLatLongHeightToXYZ(
            osg::RadiansToDegrees(lat_degrees),
            osg::RadiansToDegrees(long_degrees),
            0, 
            x, y, z);
        osg::Vec3d up  = _ellipsoidModel->computeLocalUpVector(x, y, z);
        setSunPosition( up, view );
    }
}

void
SimpleSkyNode::setMoonPosition( PerViewData& data, const osg::Vec3d& pos )
{
    if ( data._moonXform.valid() )
    {
        data._moonMatrix = osg::Matrixd::translate( pos.x(), pos.y(), pos.z() );
        data._moonXform->setMatrix( data._moonMatrix );
    }
}


void
SimpleSkyNode::onSetStarsVisible()
{
    bool visible = getStarsVisible();
    if ( _defaultPerViewData._starsXform.valid() ) 
    {
        _defaultPerViewData._starsXform->setNodeMask( visible ? ~0 : 0 );
        for( PerViewDataMap::iterator i = _perViewData.begin(); i != _perViewData.end(); ++i )
        {
            i->second._starsXform->setNodeMask( visible ? ~0 : 0 );
        }
    }
}

void
SimpleSkyNode::onSetMoonVisible()
{
    bool visible = getMoonVisible();
    if ( _defaultPerViewData._moonXform.valid() ) 
    {
        _defaultPerViewData._moonXform->setNodeMask( visible ? ~0 : 0 );
        for( PerViewDataMap::iterator i = _perViewData.begin(); i != _perViewData.end(); ++i )
        {
            i->second._moonXform->setNodeMask( visible ? ~0 : 0 );
        }
    }
}

void
SimpleSkyNode::onSetSunVisible()
{
    bool visible = getSunVisible();
    if ( _defaultPerViewData._sunXform.valid() ) 
    {
        _defaultPerViewData._sunXform->setNodeMask( visible ? ~0 : 0 );
        for( PerViewDataMap::iterator i = _perViewData.begin(); i != _perViewData.end(); ++i )
        {
            i->second._sunXform->setNodeMask( visible ? ~0 : 0 );
        }
    }
}

void
SimpleSkyNode::makeLighting()
{
    // installs the main uniforms and the shaders that will light the subgraph (terrain).
    osg::StateSet* stateset = this->getOrCreateStateSet();

    VirtualProgram* vp = VirtualProgram::getOrCreate( stateset );

    std::string vertSource = Stringify()
        << s_versionString
        << s_mathUtils
        << s_atmosphereVertexDeclarations
        << s_groundVertexShared
        << s_groundVertex;

    vp->setFunction("atmos_vertex_main", vertSource, ShaderComp::LOCATION_VERTEX_VIEW);

    std::string fragSource = Stringify()
        << s_versionString
        << s_groundFragment;

    vp->setFunction("atmos_fragment_main", fragSource, ShaderComp::LOCATION_FRAGMENT_LIGHTING, 0.0f);

    // calculate and apply the uniforms:
    // TODO: perhaps we can just hard-code most of these as GLSL consts.
    float r_wl = ::powf( .65f, 4.0f );
    float g_wl = ::powf( .57f, 4.0f );
    float b_wl = ::powf( .475f, 4.0f );
    osg::Vec3 RGB_wl( 1.0f/r_wl, 1.0f/g_wl, 1.0f/b_wl );
    float Kr = 0.0025f;
    float Kr4PI = Kr * 4.0f * osg::PI;
    float Km = 0.0015f;
    float Km4PI = Km * 4.0f * osg::PI;
    float ESun = 15.0f;
    float MPhase = -.095f;
    float RayleighScaleDepth = 0.25f;
    int   Samples = 2;
    float Weather = 1.0f;

    float Scale = 1.0f / (_outerRadius - _innerRadius);

    stateset->getOrCreateUniform( "atmos_v3InvWavelength", osg::Uniform::FLOAT_VEC3 )->set( RGB_wl );
    stateset->getOrCreateUniform( "atmos_fInnerRadius",    osg::Uniform::FLOAT )->set( _innerRadius );
    stateset->getOrCreateUniform( "atmos_fInnerRadius2",   osg::Uniform::FLOAT )->set( _innerRadius * _innerRadius );
    stateset->getOrCreateUniform( "atmos_fOuterRadius",    osg::Uniform::FLOAT )->set( _outerRadius );
    stateset->getOrCreateUniform( "atmos_fOuterRadius2",   osg::Uniform::FLOAT )->set( _outerRadius * _outerRadius );
    stateset->getOrCreateUniform( "atmos_fKrESun",         osg::Uniform::FLOAT )->set( Kr * ESun );
    stateset->getOrCreateUniform( "atmos_fKmESun",         osg::Uniform::FLOAT )->set( Km * ESun );
    stateset->getOrCreateUniform( "atmos_fKr4PI",          osg::Uniform::FLOAT )->set( Kr4PI );
    stateset->getOrCreateUniform( "atmos_fKm4PI",          osg::Uniform::FLOAT )->set( Km4PI );
    stateset->getOrCreateUniform( "atmos_fScale",          osg::Uniform::FLOAT )->set( Scale );
    stateset->getOrCreateUniform( "atmos_fScaleDepth",     osg::Uniform::FLOAT )->set( RayleighScaleDepth );
    stateset->getOrCreateUniform( "atmos_fScaleOverScaleDepth", osg::Uniform::FLOAT )->set( Scale / RayleighScaleDepth );
    stateset->getOrCreateUniform( "atmos_g",               osg::Uniform::FLOAT )->set( MPhase );
    stateset->getOrCreateUniform( "atmos_g2",              osg::Uniform::FLOAT )->set( MPhase * MPhase );
    stateset->getOrCreateUniform( "atmos_nSamples",        osg::Uniform::INT )->set( Samples );
    stateset->getOrCreateUniform( "atmos_fSamples",        osg::Uniform::FLOAT )->set( (float)Samples );
    stateset->getOrCreateUniform( "atmos_fWeather",        osg::Uniform::FLOAT )->set( Weather );
}

void
SimpleSkyNode::makeAtmosphere(const osg::EllipsoidModel* em)
{
    // create some skeleton geometry to shade:
    osg::Geometry* drawable = s_makeEllipsoidGeometry( em, _outerRadius, false );

    osg::Geode* geode = new osg::Geode();
    geode->addDrawable( drawable );
    
    // configure the state set:
    osg::StateSet* atmosSet = drawable->getOrCreateStateSet();
    atmosSet->setMode( GL_LIGHTING, osg::StateAttribute::OFF );
    atmosSet->setAttributeAndModes( new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON );
    atmosSet->setAttributeAndModes( new osg::Depth( osg::Depth::LESS, 0, 1, false ) ); // no depth write
    atmosSet->setAttributeAndModes( new osg::Depth(osg::Depth::ALWAYS, 0, 1, false) ); // no zbuffer
    atmosSet->setAttributeAndModes( new osg::BlendFunc( GL_ONE, GL_ONE ), osg::StateAttribute::ON );

    // first install the atmosphere rendering shaders.
    if ( Registry::capabilities().supportsGLSL() )
    {
        VirtualProgram* vp = VirtualProgram::getOrCreate( atmosSet );
        vp->setInheritShaders( false );

        std::string vertSource = Stringify()
            << s_versionString
            << s_mathUtils
            << s_atmosphereVertexDeclarations
            << s_atmosphereVertexShared
            << s_atmosphereVertex;

        vp->setFunction("atmos_vertex_main", vertSource, ShaderComp::LOCATION_VERTEX_VIEW);

        std::string fragSource = Stringify()
            << s_versionString
            << s_mathUtils
            << s_atmosphereFragmentDeclarations
            << s_atmosphereFragment;

        vp->setFunction("atmos_fragment_main", fragSource, ShaderComp::LOCATION_FRAGMENT_LIGHTING, 0.0f);
    }

    // A nested camera isolates the projection matrix calculations so the node won't 
    // affect the clip planes in the rest of the scene.
    osg::Camera* cam = new osg::Camera();
    cam->getOrCreateStateSet()->setRenderBinDetails( BIN_ATMOSPHERE, "RenderBin" );
    cam->setRenderOrder( osg::Camera::NESTED_RENDER );
    cam->setComputeNearFarMode( osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES );
    cam->addChild( geode );

    _atmosphere = cam;
}

void
SimpleSkyNode::makeSun()
{
    osg::Billboard* sun = new osg::Billboard();
    sun->setMode( osg::Billboard::POINT_ROT_EYE );
    sun->setNormal( osg::Vec3(0, 0, 1) );

    float sunRadius = _innerRadius * 100.0f;

    sun->addDrawable( s_makeDiscGeometry( sunRadius*80.0f ) ); 

    osg::StateSet* set = sun->getOrCreateStateSet();
    set->setMode( GL_BLEND, 1 );

    set->getOrCreateUniform( "sunAlpha", osg::Uniform::FLOAT )->set( 1.0f );

    // configure the stateset
    set->setMode( GL_LIGHTING, osg::StateAttribute::OFF );
    set->setMode( GL_CULL_FACE, osg::StateAttribute::OFF );
    set->setAttributeAndModes( new osg::Depth(osg::Depth::ALWAYS, 0, 1, false), osg::StateAttribute::ON );
    // set->setAttributeAndModes( new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON );

    // create shaders
    if ( Registry::capabilities().supportsGLSL() )
    {
        osg::Program* program = new osg::Program();
        osg::Shader* vs = new osg::Shader( osg::Shader::VERTEX, Stringify()
            << s_versionString
            << s_sunVertexSource );
        program->addShader( vs );
        osg::Shader* fs = new osg::Shader( osg::Shader::FRAGMENT, Stringify()
            << s_versionString
            << s_mathUtils
            << s_sunFragmentSource );
        program->addShader( fs );
        set->setAttributeAndModes( program, osg::StateAttribute::ON );
    }

    // make the sun's transform:
    // todo: move this?
    _defaultPerViewData._sunXform = new osg::MatrixTransform();
    _defaultPerViewData._sunXform->setMatrix( osg::Matrix::translate( 
        _sunDistance * _defaultPerViewData._lightPos.x(), 
        _sunDistance * _defaultPerViewData._lightPos.y(), 
        _sunDistance * _defaultPerViewData._lightPos.z() ) );
    _defaultPerViewData._sunXform->addChild( sun );

    // A nested camera isolates the projection matrix calculations so the node won't 
    // affect the clip planes in the rest of the scene.
    osg::Camera* cam = new osg::Camera();
    cam->getOrCreateStateSet()->setRenderBinDetails( BIN_SUN, "RenderBin" );
    cam->setRenderOrder( osg::Camera::NESTED_RENDER );
    cam->setComputeNearFarMode( osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES );
    cam->addChild( sun );

    _sun = cam;
}

void
SimpleSkyNode::makeMoon()
{
    osg::ref_ptr< osg::EllipsoidModel > em = new osg::EllipsoidModel( 1738140.0, 1735970.0 );   
    osg::Geode* moon = new osg::Geode;
    moon->getOrCreateStateSet()->setAttributeAndModes( new osg::Program(), osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED );
    osg::Geometry* geom = s_makeEllipsoidGeometry( em.get(), em->getRadiusEquator(), true );    
    //TODO:  Embed this texture in code or provide a way to have a default resource directory for osgEarth.
    //       Right now just need to have this file somewhere in your OSG_FILE_PATH
    osg::Image* image = osgDB::readImageFile( "moon_1024x512.jpg" );
    osg::Texture2D * texture = new osg::Texture2D( image );
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
    texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    texture->setResizeNonPowerOfTwoHint(false);
    geom->getOrCreateStateSet()->setTextureAttributeAndModes( 0, texture, osg::StateAttribute::ON | osg::StateAttribute::PROTECTED);

    osg::Vec4Array* colors = new osg::Vec4Array(1);    
    geom->setColorArray( colors );
    geom->setColorBinding(osg::Geometry::BIND_OVERALL);
    (*colors)[0] = osg::Vec4(1, 1, 1, 1 );
    moon->addDrawable( geom  ); 

    osg::StateSet* set = moon->getOrCreateStateSet();
    // configure the stateset
    set->setMode( GL_LIGHTING, osg::StateAttribute::ON );
    set->setAttributeAndModes( new osg::CullFace( osg::CullFace::BACK ), osg::StateAttribute::ON);
    set->setRenderBinDetails( BIN_MOON, "RenderBin" );
    set->setAttributeAndModes( new osg::Depth(osg::Depth::ALWAYS, 0, 1, false), osg::StateAttribute::ON );
    set->setAttributeAndModes( new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON );

#ifdef OSG_GLES2_AVAILABLE

    if ( Registry::capabilities().supportsGLSL() )
    {
        set->addUniform(new osg::Uniform("moonTex", 0));

        // create shaders
        osg::Program* program = new osg::Program();
        osg::Shader* vs = new osg::Shader( osg::Shader::VERTEX, Stringify()
            << s_versionString
            << s_moonVertexSource );
        program->addShader( vs );
        osg::Shader* fs = new osg::Shader( osg::Shader::FRAGMENT, Stringify()
            << s_versionString
            << s_moonFragmentSource );
        program->addShader( fs );
        set->setAttributeAndModes( program, osg::StateAttribute::ON | osg::StateAttribute::PROTECTED );
    }
#endif

    // make the moon's transform:
    // todo: move this?
    _defaultPerViewData._moonXform = new osg::MatrixTransform();    

    osg::Vec3d moonPosECEF = getEphemeris()->getMoonPositionECEF(DateTime(2011,2,1,0.0));
    _defaultPerViewData._moonXform->setMatrix( osg::Matrix::translate( moonPosECEF ) ); 
    _defaultPerViewData._moonXform->addChild( moon );

    //If we couldn't load the moon texture, turn the moon off
    if (!image)
    {
        OE_INFO << LC << "Couldn't load moon texture, add osgEarth's data directory your OSG_FILE_PATH" << std::endl;
        _defaultPerViewData._moonXform->setNodeMask( 0 );
        setMoonVisible(false);
    }

    // A nested camera isolates the projection matrix calculations so the node won't 
    // affect the clip planes in the rest of the scene.
    osg::Camera* cam = new osg::Camera();
    cam->getOrCreateStateSet()->setRenderBinDetails( BIN_MOON, "RenderBin" );
    cam->setRenderOrder( osg::Camera::NESTED_RENDER );
    cam->setComputeNearFarMode( osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES );
    cam->addChild( moon );

    _moon = cam;
}

SimpleSkyNode::StarData::StarData(std::stringstream &ss)
{
    std::getline( ss, name, ',' );
    std::string buff;
    std::getline( ss, buff, ',' );
    std::stringstream(buff) >> right_ascension;
    std::getline( ss, buff, ',' );
    std::stringstream(buff) >> declination;
    std::getline( ss, buff, '\n' );
    std::stringstream(buff) >> magnitude;
}

void
SimpleSkyNode::makeStars()
{
    _starRadius = 20000.0 * (_sunDistance > 0.0 ? _sunDistance : _outerRadius);

    std::vector<StarData> stars;

    if( _options.starFile().isSet() )
    {
        if ( parseStarFile(*_options.starFile(), stars) == false )
        {
            OE_WARN << LC 
                << "Unable to use star field defined in \"" << *_options.starFile()
                << "\", using default star data instead." << std::endl;
        }
    }

    if ( stars.empty() )
    {
        getDefaultStars( stars );
    }

    osg::Node* starNode = buildStarGeometry(stars);

    _stars = starNode;
}

osg::Node*
SimpleSkyNode::buildStarGeometry(const std::vector<StarData>& stars)
{
    double minMag = DBL_MAX, maxMag = DBL_MIN;

    osg::Vec3Array* coords = new osg::Vec3Array();
    std::vector<StarData>::const_iterator p;
    for( p = stars.begin(); p != stars.end(); p++ )
    {
        osg::Vec3d v = getEphemeris()->getECEFfromRADecl(
            p->right_ascension, 
            p->declination, 
            _starRadius );

        coords->push_back( v );

        if ( p->magnitude < minMag ) minMag = p->magnitude;
        if ( p->magnitude > maxMag ) maxMag = p->magnitude;
    }

    osg::Vec4Array* colors = new osg::Vec4Array();
    for( p = stars.begin(); p != stars.end(); p++ )
    {
        float c = ( (p->magnitude-minMag) / (maxMag-minMag) );
        colors->push_back( osg::Vec4(c,c,c,1.0f) );
    }

    osg::Geometry* geometry = new osg::Geometry;
    geometry->setUseVertexBufferObjects(true);

    geometry->setVertexArray( coords );
    geometry->setColorArray( colors );
    geometry->setColorBinding(osg::Geometry::BIND_PER_VERTEX);
    geometry->addPrimitiveSet( new osg::DrawArrays(osg::PrimitiveSet::POINTS, 0, coords->size()));

    osg::StateSet* sset = geometry->getOrCreateStateSet();

    if ( Registry::capabilities().supportsGLSL() )
    {
        sset->setTextureAttributeAndModes( 0, new osg::PointSprite(), osg::StateAttribute::ON );
        sset->setMode( GL_VERTEX_PROGRAM_POINT_SIZE, osg::StateAttribute::ON );

        std::string starVertSource, starFragSource;
        if ( Registry::capabilities().getGLSLVersion() < 1.2f )
        {
            starVertSource = s_starsVertexSource110;
            starFragSource = s_starsFragmentSource110;
        }
        else
        {
            starVertSource = s_starsVertexSource120;
            starFragSource = s_starsFragmentSource120;
        }

        osg::Program* program = new osg::Program;
        program->addShader( new osg::Shader(osg::Shader::VERTEX, starVertSource) );
        program->addShader( new osg::Shader(osg::Shader::FRAGMENT, starFragSource) );
        sset->setAttributeAndModes( program, osg::StateAttribute::ON );
    }

    sset->setRenderBinDetails( BIN_STARS, "RenderBin");
    sset->setAttributeAndModes( new osg::Depth(osg::Depth::ALWAYS, 0, 1, false), osg::StateAttribute::ON );
    sset->setMode(GL_BLEND, 1);

    osg::Geode* starGeode = new osg::Geode;
    starGeode->addDrawable( geometry );

    // A separate camera isolates the projection matrix calculations.
    osg::Camera* cam = new osg::Camera();
    cam->getOrCreateStateSet()->setRenderBinDetails( BIN_STARS, "RenderBin" );
    cam->setRenderOrder( osg::Camera::NESTED_RENDER );
    cam->setComputeNearFarMode( osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES );
    cam->addChild( starGeode );

    return cam;
    //return starGeode;
}

void
SimpleSkyNode::getDefaultStars(std::vector<StarData>& out_stars)
{
    out_stars.clear();

    for(const char **sptr = s_defaultStarData; *sptr; sptr++)
    {
        std::stringstream ss(*sptr);
        out_stars.push_back(StarData(ss));

        if (out_stars[out_stars.size() - 1].magnitude < _minStarMagnitude)
            out_stars.pop_back();
    }
}

bool
SimpleSkyNode::parseStarFile(const std::string& starFile, std::vector<StarData>& out_stars)
{
    out_stars.clear();

    std::fstream in(starFile.c_str());
    if (!in)
    {
        OE_WARN <<  "Warning: Unable to open file star file \"" << starFile << "\"" << std::endl;
        return false ;
    }

    while (!in.eof())
    {
        std::string line;

        std::getline(in, line);
        if (in.eof())
            break;

        if (line.empty() || line[0] == '#') 
            continue;

        std::stringstream ss(line);
        out_stars.push_back(StarData(ss));

        if (out_stars[out_stars.size() - 1].magnitude < _minStarMagnitude)
            out_stars.pop_back();
    }

    in.close();

    return true;
}