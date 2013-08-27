#include "Interpreter.h"
#include "VectorLayerPostgis.h"

#include <osgDB/ReadFile>
#include <osg/Material>
#include <osg/Geode>

#include <iostream>
#include <cassert>

#include <libxml/parser.h>



namespace Stack3d {
namespace Viewer {

Interpreter::Interpreter(volatile ViewerWidget * vw, const std::string & fileName )
    : _viewer( vw )
    , _inputFile( fileName )
{}

void Interpreter::run()
{
    std::ifstream ifs( _inputFile.c_str() );
    _inputFile.empty() || ifs || ERROR << "cannot open '" << _inputFile << "'";
    std::string line;
    while ( std::getline( ifs, line ) || std::getline( std::cin, line ) ) {
        if ( line.empty() || '#' == line[0] ) continue; // empty line

        std::stringstream ls(line);
        std::string cmd;
        std::getline( ls, cmd, ' ' );

        AttributeMap am;
        std::string key, value;
        while (    std::getline( ls, key, '=' ) 
                && std::getline( ls, value, '"' ) 
                && std::getline( ls, value, '"' )){
            // remove spaces in key
            key.erase( remove_if(key.begin(), key.end(), isspace ), key.end());
            am[ key ] = value;
            std::cout << "am[\"" << key << "\"]=\"" << value << "\"\n";
        }

        if ( "help" == cmd ){
            help();
        }
#define COMMAND( CMD ) \
        else if ( #CMD == cmd  ){\
            if ( !CMD( am ) ){\
               ERROR << "cannot " << #CMD;\
               std::cout << "<error msg=\""<< Log::instance().str() << "\"/>\n";\
               Log::instance().str("");\
            }\
        }
        COMMAND(loadVectorPostgis)
        COMMAND(loadRasterGDAL)
        COMMAND(loadElevation)
        COMMAND(unloadLayer)
        COMMAND(showLayer)
        COMMAND(hideLayer)
        COMMAND(setSymbology)
        COMMAND(setFullExtent)
        else{
            ERROR << "unknown command '" << cmd << "'.";
            std::cout << "<error msg=\"" << Log::instance().str() << "\"/>\n";
            Log::instance().str("");
        }
#undef COMMAND
    }
    _viewer->setDone(true);
}

inline
const std::string intToString( int i ){
    std::stringstream s;
    s << i;
    return s.str();
}

bool Interpreter::loadVectorPostgis(const AttributeMap & am )
{
    if ( am.value("id").empty() 
      || am.value("conn_info").empty() 
      || am.value("center").empty()
      ) return false;

    // with LOD
    if ( ! am.optionalValue("lod").empty() ){
        std::vector<  double > lodDistance;
        if ( am.value("extend").empty() ||  am.value("tile_size").empty() ) return false;
        std::stringstream levels(am.optionalValue("lod"));
        std::string l;
        while ( std::getline( levels, l, ' ' ) ){
           lodDistance.push_back( atof(l.c_str() ) );
           const int idx = lodDistance.size()-2;
           if (idx < 0) continue;
           const std::string lodIdx = intToString( idx );
           if ( am.value( "feature_id_"+lodIdx ).empty() 
                   || am.value("geometry_column_"+lodIdx ).empty() 
                   || am.value("query_"+lodIdx ).empty() ) return false;
        }
        
        float xmin, ymin, xmax, ymax;
        std::stringstream ext( am.value("extend") );
        if ( !(ext >> xmin >> ymin)
            || !std::getline(ext, l, ',')
            || !(ext >> xmax >> ymax) ) {
            ERROR << "cannot parse extend";
            return false;
        }

        float tileSize;
        if (!(std::stringstream(am.value("tile_size")) >> tileSize) || tileSize <= 0 ){
            ERROR << "cannot parse tile_size";
            return false;
        }

        osg::Vec3 center(0,0,0);
        if (!(std::stringstream(am.value("center")) >> center.x() >> center.y() ) ){
            ERROR << "cannot parse center";
            return false;
        }


        const size_t numTilesX = (xmax-xmin)/tileSize + 1;
        const size_t numTilesY = (ymax-ymin)/tileSize + 1;

        osg::ref_ptr<osg::Group> group = new osg::Group;
        for (size_t ix=0; ix<numTilesX; ix++){
            for (size_t iy=0; iy<numTilesY; iy++){
                osg::ref_ptr<osg::PagedLOD> pagedLod = new osg::PagedLOD;
                //pagedLod->setCenterMode(osg::PagedLOD::USE_BOUNDING_SPHERE_CENTER );
                const float xm = xmin + ix*tileSize;
                const float ym = ymin + iy*tileSize;
                for (size_t ilod = 0; ilod < lodDistance.size()-1; ilod++){
                    const std::string lodIdx = intToString( ilod );
                    const std::string query = tileQuery( am.value("query_"+lodIdx ), xm, ym, xm+tileSize, ym+tileSize );
                    if (query.empty()) return false;
                    const std::string pseudoFile = "conn_info=\""       + am.value("conn_info")       + "\" "
                                                 + "center=\""          + am.value("center")          + "\" "
                                                 + "feature_id=\""      + am.value("feature_id_"+lodIdx)      + "\" "
                                                 + "geometry_column=\"" + am.value("geometry_column_"+lodIdx) + "\" "
                                                 + "query=\""           + query + "\".postgisd";

                    pagedLod->setFileName( lodDistance.size()-2-ilod,  pseudoFile );
                    pagedLod->setRange( lodDistance.size()-2-ilod, lodDistance[ilod], lodDistance[ilod+1] );
                    std:: cout << "range " << ilod << "/" << lodIdx << ": " <<lodDistance[ilod] << "-" << lodDistance[ilod+1]<< " :" << pseudoFile << "\n";
                }
                pagedLod->setCenter( osg::Vec3( xm+.5*tileSize, ym+.5*tileSize ,0) - center );
                pagedLod->setRadius( .5*tileSize*std::sqrt(2.0) );
                group->addChild( pagedLod.get() );
            }
        }
        if (!_viewer->addNode( am.value("id"), group.get() )) return false;

        static bool once = false;
        if (!once){
            osg::Box* unitCube = new osg::Box( osg::Vec3(xmax-xmin, ymax-ymin,-2)/2, xmax-xmin, ymax-ymin, 0);
            osg::ShapeDrawable* unitCubeDrawable = new osg::ShapeDrawable(unitCube);
            osg::Geode* basicShapesGeode = new osg::Geode();
            basicShapesGeode->addDrawable(unitCubeDrawable);

            osg::Vec4Array* colours = new osg::Vec4Array(1);
            (*colours)[0].set(1.0f,1.0f,1.0,1.0f);

            osg::StateSet* stateset = new osg::StateSet;
            basicShapesGeode->setStateSet( stateset );
            osg::Material* material = new osg::Material;
            material->setAmbient(osg::Material::FRONT_AND_BACK,osg::Vec4(.2f,.2f,.2f,1.0f));
            material->setDiffuse(osg::Material::FRONT_AND_BACK,osg::Vec4(.2f,.2f,.2f,1.0f));
            stateset->setAttribute(material,osg::StateAttribute::ON);
            stateset->setMode( GL_LIGHTING, osg::StateAttribute::ON );
            stateset->setAttribute(material,osg::StateAttribute::OVERRIDE);
            _viewer->addNode( "floor", basicShapesGeode );
            once = true;
            
    }
    // without LOD
    else{
      if ( am.value("feature_id").empty() 
        || am.value("geometry_column").empty() 
        || am.value("query").empty() ) return false;
        const std::string pseudoFile = "conn_info=\""       + am.value("conn_info")       + "\" "
                                     + "center=\""          + am.value("center")          + "\" "
                                     + "feature_id=\""      + am.value("feature_id")      + "\" "
                                     + "geometry_column=\"" + am.value("geometry_column") + "\" "
                                     + "query=\""           + am.value("query")           + "\".postgisd";
        osg::ref_ptr<osg::Node> node = osgDB::readNodeFile( pseudoFile );
        if (!node.get() ){
            ERROR << "cannot create layer";
            return false;
        }
        if (!_viewer->addNode( am.value("id"), node.get() )) return false;
        }
    }


    return true;
}

bool Interpreter::loadRasterGDAL(const AttributeMap & )
{
    ERROR << "not implemented";
    return false;
}

bool Interpreter::loadElevation(const AttributeMap & )
{
    ERROR << "not implemented";
    return false;
}

bool Interpreter::unloadLayer( const AttributeMap& am )
{
    return am.value("id").empty() ? false : _viewer->removeNode( am.value("id") );
}

bool Interpreter::showLayer( const AttributeMap& am )
{
    return am.value("id").empty() ? false : _viewer->setVisible( am.value("id"), true );
}

bool Interpreter::hideLayer( const AttributeMap& am )
{
    return am.value("id").empty() ? false : _viewer->setVisible( am.value("id"), false );
}

bool Interpreter::setSymbology(const AttributeMap & )
{
    ERROR << "not implemented";
    return false;
}

bool Interpreter::setFullExtent(const AttributeMap & )
{
    ERROR << "not implemented";
    return false;
}


}
}
