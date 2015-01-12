


#ifndef POTREEWRITER_TILING1_H
#define POTREEWRITER_TILING1_H

#include <chrono>
#include <string>
#include <sstream>
#include <list>
#include <map>
#include <vector>
#include <bitset>
#include <random>

#include "boost/filesystem.hpp"

#include "AABB.h"
#include "LASPointWriter.hpp"
#include "LASPointReader.h"
#include "SparseGrid.h"
#include "stuff.h"
#include "CloudJS.hpp"
#include "PointReader.h"
#include "PointWriter.hpp"



using std::string;
using std::stringstream;
using std::chrono::high_resolution_clock;
using std::chrono::milliseconds;
using std::chrono::duration_cast;


struct MortonPoint{
	Point p;
	unsigned long long code;
	int level;

	MortonPoint(Point p, unsigned long long code){
		this->p = p;
		this->code = code;
		this->level = 0;
	}
};

bool mortonOrder(MortonPoint a, MortonPoint b){ 
	if(a.level == b.level){
		return a.code < b.code; 
	}else{
		return a.level < b.level;
	}

	

}

class PotreeWriterTiling1{

public:


	struct Node;

	struct Node{

		vector<Node*> children;
		int numPoints;
		string name;
		AABB aabb;

		Node(){
			name = "";
			numPoints = 0;
			children.resize(8, NULL);
		}

	};


	Node *root;
	AABB aabb;
	AABB tightAABB;
	string path;
	float spacing;
	int maxLevel;
	CloudJS cloudjs;
	int numAccepted;
	long long numPoints;
	int mortonDepth;
	unsigned long long mortonExtent;
	double scale;

	std::default_random_engine generator;
	std::uniform_real_distribution<double> distribution;


	vector<MortonPoint> cache;


	PotreeWriterTiling1(string path, long long numPoints, AABB aabb, float spacing, double scale, OutputFormat outputFormat){
		this->path = path;
		this->aabb = aabb;
		this->spacing = spacing;
		this->numPoints = numPoints;
		this->scale = scale;

		int target = 20*1000;
		double ratio = 4.0;

		mortonDepth = ceil((-log(ratio) + log((target - numPoints + numPoints * ratio) / target)) / log(ratio)) + 1;
		mortonExtent = pow(2, mortonDepth) ;
		maxLevel = mortonDepth - 1;

		cout << "morton depth: " << mortonDepth << endl;

		
		distribution = std::uniform_real_distribution<double>(0.0,1.0);

		root = new Node();
		root->name = "r";
		root->aabb = aabb;



		fs::remove_all(path + "/data");
		fs::remove_all(path + "/temp");
		fs::create_directories(path + "/data");
		fs::create_directories(path + "/temp");
		

		cloudjs.outputFormat = outputFormat;
		cloudjs.boundingBox = aabb;
		cloudjs.octreeDir = "data";
		cloudjs.spacing = spacing;
		cloudjs.version = "1.4";
		cloudjs.scale = scale;



	}

	unsigned long long mortonCode(Point p){

		unsigned long long mx = mortonExtent * (p.x - aabb.min.x) / (aabb.max.x - aabb.min.x);
		unsigned long long my = mortonExtent * (p.y - aabb.min.y) / (aabb.max.y - aabb.min.y);
		unsigned long long mz = mortonExtent * (p.z - aabb.min.z) / (aabb.max.z - aabb.min.z);

		unsigned long long mc = 0;
		unsigned long long mask = 1;
		for(int i = 0; i < mortonDepth; i++){
			mc = mc | (mx & mask) << (3 * i + 2 - i);
			mc = mc | (my & mask) << (3 * i + 1 - i);
			mc = mc | (mz & mask) << (3 * i + 0 - i);
			mask = mask << 1;
		}

		return mc;
	}


	// xyz
	// xzy
	// zyx


	void add(Point &p){

		MortonPoint mp(p, mortonCode(p));
		cache.push_back(mp);

	}

	

	void flush(){

		cloudjs.boundingBox = aabb;
		//cloudjs.boundingBox.min.x = aabb.min.z;
		//cloudjs.boundingBox.min.z = -aabb.min.x;
		//cloudjs.boundingBox.max.x = aabb.max.z;
		//cloudjs.boundingBox.max.z = -aabb.max.x;

		//cloudjs.boundingBox.min.y = aabb.min.z;
		//cloudjs.boundingBox.min.z = -aabb.min.y;

		cloudjs.tightBoundingBox = cloudjs.boundingBox;
		cloudjs.outputFormat = OutputFormat::LAS;
		cloudjs.spacing = spacing;
		cloudjs.scale = scale;

		//sort(cache.begin(), cache.end(), mortonOrder);

		static int c = 0;

		map<unsigned long long, LASPointWriter*> writers;

		auto startSort = high_resolution_clock::now();
		double gs = ( 1.0 - pow(4.0, (double)maxLevel + 1.0) ) / (1.0 - 4.0);
		vector<double> props;
		for(int i = 0; i <= maxLevel; i++){
			double p = pow(4.0, (double)i) / gs;

			if(i > 0){
				p = p + props[i-1];
			}

			props.push_back(p);
		}

		
		for(int i = 0; i < cache.size(); i++){

			MortonPoint &mp = cache[i];

			//double r = (double)rand() / RAND_MAX;
			double r = distribution(generator);
			for(int j = 0; j <= maxLevel; j++){
				if(r < props[j]){
					mp.level = j;
					break;
				}
			}


			unsigned long long mc = mp.code;
			mc = mc >> (3 * (mortonDepth - mp.level) );
			mc = mc << (3 * (mortonDepth - mp.level) );
			if(mp.level == 0){
				mc = -1;
			}
			mp.code = mc;
		}
		sort(cache.begin(), cache.end(), mortonOrder);
		
		auto endSort = high_resolution_clock::now();
		milliseconds duration = duration_cast<milliseconds>(endSort-startSort);
		cout << "sort: " << (duration.count()/1000.0f) << "s" << endl;

		auto startWrite = high_resolution_clock::now();

		int prevMc = -2;
		int prevLevel = -1;
		LASPointWriter *writer = NULL;
		Node *currentNode = NULL;
		for(int i = 0; i < cache.size(); i++){
			MortonPoint &mp = cache[i];
			unsigned long long mc = mp.code;

			//cout << "=====" << endl;
			//cout << "point: " << mp.p.position() << endl;
			//cout << "mc: " << mp.code << endl;
			//cout << "level: " << mp.level << endl;

			if(writer == NULL || mc != prevMc || prevLevel != mp.level){

				currentNode = root;
			
				if(writer != NULL){
					writer->close();
					delete writer;
				}
			
				stringstream ssName;
				ssName << "r";
				for(int j = 1; j <= mp.level; j++){
					int index = (mc >> ( 3 * ( mortonDepth - j ) )) & 7;

					ssName << index;

					if(currentNode->children[index] == NULL){
						Node *child = new Node();
						child->aabb = childAABB(currentNode->aabb, index);
						child->name = ssName.str();
						currentNode->children[index] = child;				
					}
					currentNode = currentNode->children[index];	
				}
				stringstream ssFile;
				ssFile << path << "/data/" << ssName.str() << ".las";
			
				writer = new LASPointWriter(ssFile.str(), currentNode->aabb, scale);
			}

			currentNode->numPoints++;
			writer->write(mp.p);

			prevMc = mc;
			prevLevel = mp.level;

		}
		if(writer != NULL){
			writer->close();
			delete writer;
		}

		auto endWrite = high_resolution_clock::now();
		duration = duration_cast<milliseconds>(endWrite-startWrite);
		cout << "write: " << (duration.count()/1000.0f) << "s" << endl;


		cache.clear();
		c++;


		cloudjs.hierarchy.clear();

		vector<Node*> stack;
		stack.push_back(root);
		int pos = 0;
		while(pos < stack.size()){
			Node *current = stack[pos];

			for(int j = 0; j < 8; j++){
				if(current->children[j] != NULL){
					stack.push_back(current->children[j]);
				}
			}

			cloudjs.hierarchy.push_back(CloudJS::Node(current->name, current->numPoints));

			stringstream filename;
			filename << path << "/data/" << current->name << ".las";
			if(!fs::exists(fs::path(filename.str()))){
				LASPointWriter *writer = new LASPointWriter(filename.str(), current->aabb, scale);
				writer->close();
				delete writer;
			}


			pos++;
		}

		ofstream cloudOut(path + "/cloud.js", ios::out);
		cloudOut << cloudjs.getString();
		cloudOut.close();
		

	}

	void close(){
		flush();




	}




};


#endif