/*
 * SQUARE-1 OPTIMIZER version 3.0
 * by Jaap Scherphuis, jaapsch@yahoo.com, copyright 2003-2011 (v1)
 * Michael Gottlieb, qqwref@gmail.com, copyright 2023-2024 (v2)
 * Abid ibn Ashraf and Matt Mao, squango.support@gmail.com, copyleft 2026 :)
 */

#include <fstream>
#include <iostream>
#include <cstring>
#include <ctime>
#include <vector>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <map>

#define NUMHALVES 13
#define NUMLAYERS 158
#define NUMSHAPES 7356
#define FILESTT "sq1stt.dat"
#define FILESCTE "sq1scte.dat"
#define FILESCTC "sq1sctc.dat"
#define FILEP1U  "sq1p1u.dat"
#define FILEP2U  "sq1p2u.dat"
#define FILEP1W  "sq1p1w.dat"
#define FILEP2W  "sq1p2w.dat"
#define FILEP1A  "sq1p1a.dat"
#define FILEP2A  "sq1p2a.dat"

#define TURN_METRIC 0
#define SLICE_METRIC 1
#define ANGLE_METRIC 2

const char* errors[]={
	"Unrecognized command line switch.", //1
	"Too many command line arguments.",
	"Input file not found.",//3
	"Bracket ) expected.",//4
	"Bottom layer turn expected.",//5
	"Comma expected.",//6
	"Top layer turn expected.",//7
	"Bracket ( expected.",//"8
	"Position should be 16 or 17 characters.",//9
	"Expected A-H or 1-8.",//10
	"Expected - or /.",//11
	"Slice is blocked by corner.",//12
	"Can't parse input as position string or movelist.",//13
	"Unexpected bracket (.",//14
	"Number expected.",//15
	"Slice / expected.",//16
	"Position string has too many copies of a piece.",//17
	"Can't stay in cube shape and also use 2gen.",//18
	"Position can't be solved with these constraints",//19
};

#include "karnotation.h"
#include "sq1-logic.h"

int verbosity = 5;
bool generator=false;
bool usenegative=false;
bool usebrackets=false;
int karnotation = 0;  // 0=off, 1=plain karn, 2=smart karn
bool specificAngleTop=false;
bool specificAngleBot=false;
int metric = SLICE_METRIC;
int maxX = 6;
int maxY = 6;
int maxTotal = 12;
std::vector<int> specificDepths;

static std::string tableDirectory = ".";
static std::atomic_bool stopRequested{false};
static bool s_hasInjectedPosition = false;
static int  s_injectedPos[24];
static int  s_injectedMiddle = 1;
static bool g_extendedOutput = false;

void sq1optSetExtendedOutput(bool val) { g_extendedOutput = val; }

void sq1optSetTableDirectory(const std::string& dir)
{
	tableDirectory = dir.empty() ? "." : dir;
}

void sq1optRequestStop()
{
	stopRequested.store(true);
}

void sq1optSetPosition(const int pos[24], int middle)
{
	for (int i = 0; i < 24; i++) s_injectedPos[i] = pos[i];
	s_injectedMiddle = middle;
	s_hasInjectedPosition = true;
}

// Stable C ABI entry points used by the browser build.  Keeping these wrappers
// here means the WASM proof of concept calls the same solver as the desktop app.
extern "C" {
void sq1opt_web_set_table_directory(const char* dir)
{
	 sq1optSetTableDirectory(dir ? std::string(dir) : std::string());
}

void sq1opt_web_request_stop()
{
	sq1optRequestStop();
}
}

static std::string tablePath(const char* fileName)
{
	if (tableDirectory == "." || tableDirectory.empty()) return fileName;
	const char last = tableDirectory[tableDirectory.size() - 1];
	if (last == '/' || last == '\\') return tableDirectory + fileName;
	return tableDirectory + "/" + fileName;
}

static void resetSolverOptions()
{
	stopRequested.store(false);
	verbosity = 5;
	generator = false;
	usenegative = true;
	usebrackets = false;
	karnotation = 0;
	specificAngleTop = false;
	specificAngleBot = false;
	metric = SLICE_METRIC;
	maxX = 6;
	maxY = 6;
	maxTotal = 12;
	specificDepths.clear();
}

static inline void throwIfStopped()
{
	if (stopRequested.load()) throw std::runtime_error("Solver stopped.");
}

class HalfLayer {
public:
	int pieces, turn, nPieces;
	HalfLayer(int p, int t) {
		int nEdges=0;
		pieces = p;
		for(int i=0, m=1; i<6; i++, m<<=1){
			if( (pieces&m)!=0 ) nEdges++;
		}
		nPieces=3+nEdges/2;
		turn=t;
	}
};

class Layer {
public:
	HalfLayer& h1, & h2;
	int turnt, turnb;
	int nPieces;
	bool turnParityOdd;
	bool turnParityOddb;
	int pieces;
	int tpieces, bpieces;   // result after turn

	Layer( HalfLayer& p1, HalfLayer& p2): h1(p1), h2(p2) {
		pieces = (h1.pieces<<6)+h2.pieces;
		nPieces = h1.nPieces + h2.nPieces;

		int m=1;
		for(turnt=1; turnt<6; turnt++){
			if( (h1.turn&h2.turn&m)!=0 ) break;
			m<<=1;
		}
		if( turnt==6 ) turnb=6;
		else{
			m=1<<4;
			for(turnb=1; turnb<5; turnb++){
				if( (h1.turn&h2.turn&m)!=0 ) break;
				m>>=1;
			}
		}

		tpieces=pieces;
		int nEdges=0;
		for( int i=0; i<turnt; i++ ){
			if( (tpieces&1)!=0 ) { tpieces+=(1<<12); nEdges++; }
			tpieces>>=1;
		}
		// find out parity of the layer turn
		// Is odd cycle if even # pieces, and odd number passes seam
		// (Note (turn+edges)/2 = number of pieces crossing seam)
		turnParityOdd = (nPieces&1)==0 && ((turnt+nEdges)&2)!=0;

		bpieces=pieces;
		nEdges=0;
		for( int i=0; i<turnb; i++ ){
			bpieces<<=1;
			if( (bpieces&(1<<12))!=0 ) { bpieces-=(1<<12)-1; nEdges++; }
		}
		// ditto
		turnParityOddb = (nPieces&1)==0 && ((turnb+nEdges)&2)!=0;

	}
};

class Sq1Shape {
public:
	Layer& topl, &botl;
	int pieces;
	bool parityOdd;
	int tpieces[4];
	bool tparity[4];
	Sq1Shape( Layer& l1, Layer& l2, bool p) : topl(l1), botl(l2) {
		parityOdd=p;
		pieces = (l1.pieces<<12)+l2.pieces;
		tpieces[0] = (l1.tpieces<<12)+l2. pieces;
		tpieces[1] = (l1. pieces<<12)+l2.bpieces;
		tpieces[2] = (l1.h1.pieces<<18)+(l2.h1.pieces<<12)+(l1.h2.pieces<<6)+(l2.h2.pieces);
		// calculate mirrored shape
		tpieces[3] = 0;
		for( int m=1, i=0; i<24; i++,m<<=1){
			tpieces[3]<<=1;
			if( (pieces&m)!=0 ) tpieces[3]++;
		}
		tparity[0] = parityOdd^l1.turnParityOdd;
		tparity[1] = parityOdd^l2.turnParityOddb;
		tparity[2] = parityOdd^( (l1.h2.nPieces&1)!=0 && (l2.h1.nPieces&1)!=0 );
		tparity[3] = parityOdd;
	}
};


class ChoiceTable {
public:
	unsigned char choice2Idx[256];
	unsigned char idx2Choice[70];
	ChoiceTable(){
		unsigned char nc=0;
		for( int i=0; i<255; i++ ) choice2Idx[i]=255;
		for( int i=1; i<255; i<<=1 ){
			for( int j=i+i; j<255; j<<=1 ){
				for( int k=j+j; k<255; k<<=1 ){
					for( int l=k+k; l<255; l<<=1 ){
						choice2Idx[i+j+k+l]=nc;
						idx2Choice[nc++]=(unsigned char)(i+j+k+l);
					}
				}
			}
		}
	}
};


class ShapeTranTable {
public:
	int nShape;
	Sq1Shape* shapeList[NUMSHAPES];
	int (*tranTable)[4];
	HalfLayer* hl[NUMHALVES];
	Layer* ll[NUMLAYERS];

	ShapeTranTable(){
		//first build list of possible halflayers
		int hi[]={ 0,    3,12,48, 9,36,33,  15,39,51,57,60,  63};
		int ht[]={42,   43,46,58,45,54,53,  47,55,59,61,62,  63};
		for( int i=0; i<NUMHALVES; i++ ){ hl[i]=new HalfLayer(hi[i],ht[i]); }

		//Now build list of possible Layers
		int lll=0;
		for( int i=0; i<NUMHALVES; i++ ){
			for( int j=0; j<NUMHALVES; j++ ){
				if( hl[i]->nPieces + hl[j]->nPieces<=10 ){
					ll[lll++]=new Layer( *hl[i], *hl[j] );
				}
			}
		}

		//Now build list of all possible shapes
		nShape=0;
		for( int i=0; i<lll; i++ ){
			for( int j=0; j<lll; j++ ){
				if( ll[i]->nPieces + ll[j]->nPieces==16 ){
					shapeList[nShape++]=new Sq1Shape( *ll[i], *ll[j], true );
					shapeList[nShape++]=new Sq1Shape( *ll[i], *ll[j], false );
				}
			}
		}

		// At last we can calculate full transition table
		tranTable = new int[NUMSHAPES][4];
		// see if can be found on file
		std::ifstream is(tablePath(FILESTT), std::ios::binary);
		if( is.fail() ){
			// no file. calculate table.
			for( int i=0; i<nShape; i++ ){
				throwIfStopped();
				//effect on shape of each move, including reflection
				for( int m=0; m<4; m++ ){
					for( int j=0; j<nShape; j++ ){
						if( shapeList[i]->tpieces[m] == shapeList[j]->pieces &&
							shapeList[i]->tparity[m] == shapeList[j]->parityOdd ){
							tranTable[i][m]=j;
							break;
						}
					}
				}
			}
			// save to file
			std::ofstream os(tablePath(FILESTT), std::ios::binary);
			os.write( (char*)tranTable, nShape*4*sizeof(int) );
		}else{
			// read from file
			nShape = NUMSHAPES;
			is.read( (char*)tranTable, nShape*4*sizeof(int) );
		}
	}
	~ShapeTranTable(){
		for( int i=0; i<NUMHALVES; i++ ){ delete hl[i]; }
		for( int i=0; i<NUMLAYERS; i++ ){ delete ll[i]; }
		for( int i=0; i<nShape; i++ ){ delete shapeList[i]; }
		delete[] tranTable;
	}
	inline int getShape(int s, bool p){
		for( int i=0; i<nShape; i++){
			if( shapeList[i]->pieces == s && shapeList[i]->parityOdd==p ) return i;
		}
		return -1;
	}
	inline int getTopTurn(int s){
		return shapeList[s]->topl.turnt;
	}
	inline int getBotTurn(int s){
		return shapeList[s]->botl.turnb;
	}
};

class ShapeColPos {
	ShapeTranTable &stt;
	ChoiceTable &ct;
	int shapeIx;
	int coloring; // 24bit string
	bool edgesFlag;
public:
	ShapeColPos( ShapeTranTable& stt0, ChoiceTable& ct0)
		: stt(stt0), ct(ct0) {}
	void set( int shp, int col, bool edges )
	{
		// col is 8 bit coloring of one type of piece.
		// edges set then edge coloring, else corner coloring
		// get full 24 bit coloring.
		int c=ct.idx2Choice[col];
		shapeIx = shp;
		edgesFlag = edges;
		coloring=0;
		int s=stt.shapeList[shapeIx]->pieces;
		if( edges ){
			for( int m=1, i=0, n=1; i<24; m<<=1, i++){
				if( (s&m)!=0 ) {
					if( (c&n)!=0 ) coloring |= m;
					n<<=1;
				}
			}
		}else{
			for( int m=3, i=0, n=1; i<24; m<<=1, i++){
				if( (s&m)==0 ) {
					if( (c&n)!=0 ) coloring |= m;
					n<<=1;
					m<<=1; i++;
				}
			}
		}
	}
	void domove(int m){
		const int botmask = (1<<12)-1;
		const int topmask = (1<<24)-(1<<12);
		const int botrmask = (1<<12)-(1<<6);
		const int toprmask = (1<<18)-(1<<12);
		const int leftmask = botmask+topmask-botrmask-toprmask;
		if( m==0 ){
			int tn=stt.getTopTurn(shapeIx);
			int b=coloring&botmask;
			int t=coloring&topmask;
			t+=(t>>12);
			t<<=(12-tn);
			coloring = b + (t&topmask);
		}else if( m==1 ){
			int tn=stt.getBotTurn(shapeIx);
			int b=coloring&botmask;
			int t=coloring&topmask;
			b+=(b<<12);
			b>>=(12-tn);
			coloring = t + (b&botmask);
		}else if( m==2 ){
			int b=coloring&botrmask;
			int t=coloring&toprmask;
			coloring = (coloring&leftmask) + (t>>6) + (b<<6);
		}
		shapeIx=stt.tranTable[shapeIx][m];
	}
	unsigned char getColIdx(){
		int c=0,n=1;
		int s=stt.shapeList[shapeIx]->pieces;
		if( edgesFlag ){
			for( int m=1, i=0; i<24; m<<=1, i++){
				if( (s&m)!=0 ) {
					if( (coloring&m)!=0 ) c |= n;
					n<<=1;
				}
			}
		}else{
			for( int m=3, i=0; i<24; m<<=1, i++){
				if( (s&m)==0 ) {
					if( (coloring&m)!=0 ) c |= n;
					n<<=1;
					m<<=1; i++;
				}
			}
		}
		return(ct.choice2Idx[c]);
	}
};



class ShpColTranTable {
public:
	char (*tranTable)[70][3];
	ShapeTranTable& stt;
	ChoiceTable& ct;

	ShpColTranTable( ShapeTranTable& stt0, ChoiceTable& ct0, bool edges )
		: stt(stt0), ct(ct0)
	{
		ShapeColPos p(stt,ct);
		tranTable = new char[NUMSHAPES][70][3];

		// see if can be found on file
		std::ifstream is(tablePath(edges? FILESCTE : FILESCTC), std::ios::binary);
		if( is.fail() ){
			// no file. calculate table.
			// Calculate transition table
			int i,j,m;
			for( m=0; m<3; m++ ){
				for( i=0; i<NUMSHAPES; i++ ){
					throwIfStopped();
					for( j=0; j<70; j++){
						p.set(i,j,edges);
						p.domove(m);
						tranTable[i][j][m]=p.getColIdx();
						if( p.getColIdx()==255 ){
							throw std::runtime_error("Invalid shape/color transition table entry.");
						}
					}
				}
			}
			// save to file
			std::ofstream os(tablePath(edges? FILESCTE : FILESCTC), std::ios::binary);
			os.write( (char*)tranTable, NUMSHAPES*3*70*sizeof(char) );
		}else{
			// read from file
			is.read( (char*)tranTable, NUMSHAPES*3*70*sizeof(char) );
		}
	}
	~ShpColTranTable(){
		delete[] tranTable;
	}
};

/*
 * Find valid preADFs for 2-gen (2g) / pseudo-2-gen (p2g).
 *
 * A valid preADF is a doBot() amount that rotates a solved block into DL
 * (pos[18..23], i.e. offsets 6..11 after the rotation).
 *
 * 2g: any solved CECE/ECEC must land in DL.
 * p2g: any solved CEC block can land at offsets 7..11 or 6..10
 * none: returns {0} (no preADF).
 *
 * after doBot(k): new pos[12+i] == old pos[12 + (i−k+12)%12].
 *
 * declared in sq1opt-runner.h.
 */

// whether a piece value (concrete or partial) could represent a concrete piece `target`
bool couldBe(int posVal, int target) {
	if (posVal == target) return true;
	if (posVal>15 && posVal%3==0  && target>=8  && target<=11) return true; // top edge (X)
	if (posVal>15 && posVal%3==1  && target>=12 && target<=15) return true; // bot edge (Y)
	if (posVal<0  && posVal%3==0  && target>=0  && target<=3)  return true; // top corner (U)
	if (posVal<0  && posVal%3==-2 && target>=4  && target<=7)  return true; // bot corner (V)
	if (posVal>15 && posVal%3==2  && target>=8  && target<=15) return true; // any edge (Z)
	if (posVal<0  && posVal%3==-1 && target>=0  && target<=7)  return true; // any corner (W)
	return false;
}

/**
 * Validity of pos[24]
 * (same as Sq1Widget::setPositionFromString and FullPosition::parseInput)
 *
 * 1. concrete pieces appear at most once
 * 2. at most 4 C/E of each color
 *
 * Assumes a well-formed array (corner halves adjacent).
 */
bool validPosition(const int pos[24]) {
	int pieceCount[16] = {0};
	int cUp=0, cDown=0, cTot=0, eUp=0, eDown=0, eTot=0;
	for (int i=0; i<24; i++) {
		int k = pos[i];
		if (k>=0 && k<=15) { if (++pieceCount[k] > 1) return false; }
		if (k<8) {
			cTot++;
			if ((k<0 && k%3==0)  || (k>=0 && k<=3)) cUp++;
			if ((k<0 && k%3==-2) || (k>=4 && k<=7)) cDown++;
			i++;
		} else {
			eTot++;
			if ((k>15 && k%3==0) || (k>=8  && k<=11)) eUp++;
			if ((k>15 && k%3==1) || (k>=12 && k<=15)) eDown++;
		}
	}
	if (cUp>4 || cDown>4 || cTot>8 || eUp>4 || eDown>4 || eTot>8) return false;
	return true;
}

std::vector<int> twoGenPreADF(const int pos[24], int twoGen, bool specificAngleBot = false, bool firstMatchOnly = false) {
	std::vector<int> result;
	if (twoGen == 0) { result.push_back(0); return result; }

	// all 8 possible CECE/ECEC blocks
	static const int blocks2g[8][6] = {
		{14, 6, 6,15, 7, 7}, // 7G8H (solved DL)
		{15, 7, 7,12, 4, 4}, // 8H5E
		{12, 4, 4,13, 5, 5}, // 5E6F
		{13, 5, 5,14, 6, 6}, // 6F7G
		{ 6, 6,15, 7, 7,12}, // G8H5
		{ 7, 7,12, 4, 4,13}, // H5E6
		{ 4, 4,13, 5, 5,14}, // E6F7
		{ 5, 5,14, 6, 6,15}, // F7G8
	};
	// all 4 possible CEC blocks
	static const int blocksP2g[4][5] = {
		{4,4,13,5,5}, // E6F
		{5,5,14,6,6}, // F7G
		{6,6,15,7,7}, // G8H
		{7,7,12,4,4}, // H5E
	};

	/*
	 * k = D move
	 * realIdx(i) = pos[] index that lands at bottom offset i after doBot(k).
	 * being valid = couldBe() W AND that leaves a valid state
	 */
	for (int k = 0; k < 12; k++) {
		if (specificAngleBot && k != 0 && k != 1 && k != 11) continue;
		auto realIdx = [&](int i) { return 12 + (i - k + 12) % 12; };
		bool ok = false;

		if (twoGen == 2) {
			for (const auto& W : blocks2g) {
				bool match = true;
				for (int j=0; j<6; j++) if (!couldBe(pos[realIdx(6+j)], W[j])) { match=false; break; }
				if (!match) continue;
				int copy[24]; for (int i=0;i<24;i++) copy[i]=pos[i];
				for (int j=0; j<6; j++) copy[realIdx(6+j)] = W[j];
				if (validPosition(copy)) { ok=true; break; }
			}
		} else {
			for (const auto& W : blocksP2g) {
				for (int base : {7, 6}) {
					bool match = true;
					for (int j=0; j<5; j++) if (!couldBe(pos[realIdx(base+j)], W[j])) { match=false; break; }
					if (!match) continue;
					int copy[24]; for (int i=0;i<24;i++) copy[i]=pos[i];
					for (int j=0; j<5; j++) copy[realIdx(base+j)] = W[j];
					if (validPosition(copy)) { ok=true; break; }
				}
				if (ok) break;
			}
		}
		if (ok) {
			result.push_back(k);
			if (firstMatchOnly) return result;
		}
	}
	return result;
}

static inline void colorShift24(int out[24], const int in[24], int aufAmt, int adfAmt) {
	for (int i = 0; i < 24; i++) {
		int v = in[i];
		if      (v >= 0  && v <= 3)  out[i] = (v + aufAmt) & 3;
		else if (v >= 8  && v <= 11) out[i] = 8  + ((v - 8  + aufAmt) & 3);
		else if (v >= 4  && v <= 7)  out[i] = 4  + ((v - 4  + adfAmt) & 3);
		else if (v >= 12 && v <= 15) out[i] = 12 + ((v - 12 + adfAmt) & 3);
		else out[i] = v;
	}
}

// smallest of the 16 color-shifted variants for comparisons
static std::array<int,24> canonicalcolorForm(const int pos[24]) {
	std::array<int,24> best{};
	int shifted[24];
	for (int t = 0; t < 4; t++) {
		for (int b = 0; b < 4; b++) {
			colorShift24(shifted, pos, t, b);
			std::array<int,24> cand;
			for (int i = 0; i < 24; i++) cand[i] = shifted[i];
			if ((t == 0 && b == 0) || cand < best) best = cand;
		}
	}
	return best;
}

// magnitude of preABF increases by absolute value
static const std::vector<int>& preABFRotationOrder() {
	static const std::vector<int> order = {0,-1,1,-2,2,-3,3,-4,4,-5,5,-6};
	return order;
}

static inline void rotateSlots(int arr[24], int lo, int m) {
	m %= 12; if (m < 0) m += 12;
	while (m-- > 0) {
		int c = arr[lo + 11];
		for (int i = 11; i > 0; i--) arr[lo + i] = arr[lo + i - 1];
		arr[lo] = c;
	}
}

// Enumerates every preABF pair, applies the 2-gen preADF constraint,
// and dedups by color-shift. Sorted ascending by abf amount.
std::vector<std::pair<int,int>> symmetricPreABF(const int origPos[24], int twoGen, bool specificAngleBot, bool specificAngleTop) {
	std::vector<int> adfAllowed;
	if (twoGen != 0) adfAllowed = twoGenPreADF(origPos, twoGen, specificAngleBot, false);

	struct Cand { int auf, adf; };
	std::vector<Cand> cands;
	for (int auf : preABFRotationOrder()) {
		if (specificAngleTop && auf != 0 && auf != 1 && auf != -1) continue;
		for (int adf : preABFRotationOrder()) {
			if (specificAngleBot && adf != 0 && adf != 1 && adf != -1) continue;
			if (twoGen != 0) {
				int adfNorm = ((adf % 12) + 12) % 12;
				if (std::find(adfAllowed.begin(), adfAllowed.end(), adfNorm) == adfAllowed.end()) continue;
			}
			cands.push_back({auf, adf});
		}
	}
	std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
		auto absi = [](int x){ return x < 0 ? -x : x; };
		return (absi(a.auf) + absi(a.adf)) < (absi(b.auf) + absi(b.adf));
	});

	std::map<int, std::vector<std::array<int,24>>> seenByShape;
	std::vector<std::pair<int,int>> result;
	for (const auto& c : cands) {
		int work[24];
		for (int i = 0; i < 24; i++) work[i] = origPos[i];
		rotateSlots(work, 0, c.auf);
		rotateSlots(work, 12, c.adf);
		if (!(work[0]!=work[11] && work[5]!=work[6] && work[12]!=work[23] && work[17]!=work[18])) continue; // not sliceable
		int shape = 0;
		for (int i = 0; i < 24; i++) if (work[i] >= 8) shape |= (1 << (23 - i));
		auto canon = canonicalcolorForm(work);
		auto& bucket = seenByShape[shape];
		bool dup = false;
		for (const auto& prev : bucket) if (prev == canon) { dup = true; break; }
		if (dup) continue;
		bucket.push_back(canon);
		result.push_back({c.auf, c.adf});
	}
	return result;
}

/**
 * Whether the corner permutation is reachable with 2-gen.
 *
 * declared in sq1opt-runner.h.
 */
bool has2GenCorners(const int pos[24]) {
	// get corners
	int tmp[6];
	int j=0;
	for (int i=0; i<18; i++) {
		if (pos[i]<8) {
			if (j%2 == 0) tmp[j/2] = pos[i];
			j++;
		}
	}
	// AUF and then insert a D corner on U, if there is one.
	int found_d = -1;
	for (int i=0; i<4; i++) if(tmp[i]>3) found_d = i;
	if (found_d > -1) {
		int tmp2[4];
		for (int i=0; i<4; i++) tmp2[i] = tmp[i];
		for (int i=0; i<4; i++) tmp[i] = tmp2[(i + found_d) % 4];
		int k = tmp[0]; tmp[0] = tmp[4]; tmp[4] = k;
		k = tmp[2]; tmp[2] = tmp[3]; tmp[3] = k;
	}
	found_d = -1;
	for (int i=0; i<4; i++) if(tmp[i]>3) found_d = i;
	if (found_d > -1) {
		int tmp2[4];
		for (int i=0; i<4; i++) tmp2[i] = tmp[i];
		for (int i=0; i<4; i++) tmp[i] = tmp2[(i + found_d) % 4];
		int k = tmp[0]; tmp[0] = tmp[5]; tmp[5] = k;
		k = tmp[1]; tmp[1] = tmp[2]; tmp[2] = k;
	}
	// NJ if D corners are swapped
	if (tmp[4] == 5 && tmp[5] == 4) {
		tmp[4] = 4; tmp[5] = 5;
		int k = tmp[0]; tmp[0] = tmp[2]; tmp[2] = k;
	}
	int found_u = -1;
	for (int i=0; i<4; i++) if(tmp[i]==0) found_u = i;
	if (found_u > -1) {
		int tmp2[4];
		for (int i=0; i<4; i++) tmp2[i] = tmp[i];
		for (int i=0; i<4; i++) tmp[i] = tmp2[(i + found_u) % 4];
	}
	if (tmp[0] == 0 && tmp[1] == 1 && tmp[2] == 2 && tmp[3] == 3 && tmp[4] == 4 && tmp[5] == 5) return true;
	return false;
}

/**
 * partial version of has2GenCorners
 * (DFL and DBL should be solved first)
 * 1. looks at the 6 corners other than DFL and DBL
 * 1. Resolves partial corners by elimination
 * 2. then defers to the concrete has2GenCorners:
 * 		0 partials   -> plain has2GenCorners
 * 		>=3 partials -> always true
 * 		1-2 partials -> OR all ways of concrete completions (respect partial's layers)
 */
bool partialHas2GenCorners(const int pos[24]) {
	int slot[6];          // slot index for the 6 corners
	int ptype[6];         // partial type: 0=U(top), 1=V(bottom), 2=W(any), -1=concrete
	bool present[8] = {false}; // which concrete corners are present among the 6?
	int n = 0;
	for (int i = 0; i < 18 && n < 6; i++) {
		if (pos[i] < 8) { // corner
			int v = pos[i];
			slot[n] = i;
			if (v < 0) ptype[n] = (v % 3 == 0) ? 0 : (v % 3 == -2 ? 1 : 2);
			else { ptype[n] = -1; present[v] = true; }
			n++;
			i++;
		}
	}

	int p[2], numPartial = 0;
	for (int t = 0; t < n; t++) if (ptype[t] >= 0) { if (numPartial < 2) p[numPartial] = t; numPartial++; }

	if (numPartial == 0) return has2GenCorners(pos);
	if (numPartial >= 3) return true;

	// unused concrete piece: non-6/7 & not `present`
	int avail[6], nA = 0;
	for (int c = 0; c < 6; c++) if (!present[c]) avail[nA++] = c;
	if (nA != numPartial) return false; // inconsistent input

	// A corner is compatible with a partial slot iff it lies in the slot's layer:
	// U -> 0-3, V -> 4-7, W -> anything.
	auto compat = [](int type, int val) {
		if (type == 0) return val >= 0 && val <= 3;
		if (type == 1) return val >= 4 && val <= 7;
		return true;
	};
	auto tryAssign = [&](int v0, int v1) -> bool {
		if (!compat(ptype[p[0]], v0)) return false;
		if (numPartial == 2 && !compat(ptype[p[1]], v1)) return false;
		int copy[24]; for (int i = 0; i < 24; i++) copy[i] = pos[i];
		copy[slot[p[0]]] = v0; copy[slot[p[0]] + 1] = v0;
		if (numPartial == 2) { copy[slot[p[1]]] = v1; copy[slot[p[1]] + 1] = v1; }
		return has2GenCorners(copy);
	};

	if (numPartial == 1) return tryAssign(avail[0], 0);
	return tryAssign(avail[0], avail[1]) || tryAssign(avail[1], avail[0]);
}

/**
 * "Do any preADF candidate of this position have 2g corners?"
 * For each preADF k, we do it, and then color shift it to be G,H (UVWXYZ unchanged).
 * specificAngleBot = lock k to -1, 0, 1
 */
bool cornersAre2GenSolvable(const int pos[24], int twoGen, bool specificAngleBot = false) {
	if (twoGen == 0) return true;
	static const int C[24] = {0,0,8,1,1,9,2,2,10,3,3,11,12,4,4,13,5,5,14,6,6,15,7,7}; // solved
	auto doBotArr = [](int a[24], int m){
		m = ((m % 12) + 12) % 12;
		while (m-- > 0) { int c = a[23]; for (int i=23;i>12;i--) a[i]=a[i-1]; a[12]=c; }
	};
	for (int k : twoGenPreADF(pos, twoGen, specificAngleBot)) {
		int copy[24]; for (int i=0;i<24;i++) copy[i]=pos[i]; doBotArr(copy, k);
		int cano; // the amount to color shift by
		if (copy[23] >= 0 && copy[23] < 8) cano = (7 - copy[23]) * 3; // corner
		else cano = (7 - copy[22]) * 3; // last piece was an edge, take [22] instead
		int Ck[24]; for (int i=0;i<24;i++) Ck[i]=C[i]; doBotArr(Ck, cano);
		int sigma[8]; for (int i=0;i<8;i++) sigma[i]=i; // sigma: the color shift mapping
		for (int i=0;i<24;i++) if (Ck[i] >= 0 && Ck[i] < 8) sigma[Ck[i]] = C[i];
		for (int i=0;i<24;i++) if (copy[i] >= 0 && copy[i] < 8) copy[i] = sigma[copy[i]];
		if (partialHas2GenCorners(copy)) return true;
	}
	return false;
}

/**
 * 2-gen compatibility / preADF enumeration.
 * Enumerate all possible positions from partial/duplicate-piece states,
 * and then calls functions above.
 * Input (matches solver.ts rawPosition(); different from FullPosition):
 * 		concrete/UVWXYZ   unchanged
 * 		<= -1000          duplicate declaration of corner piece p (0-7): -1000 - p
 * 		>= 1000           duplicate declaration of edge piece p (8-15):   1000 + p
 */
namespace TwoGenExact {

struct OpenSlot {
	int index;   // pos[] index
	bool corner;
	int layer;   // 0 = top, 1 = bottom, 2 = any
};

struct DupGroup {
	int piece;                // 0-15
	std::vector<int> members; // all pos[] indices defined with this piece
};

// OR the results of `visit` on all concrete completions of `pos`
template <typename Visit>
bool forEachCompletion(const int pos[24], Visit&& visit) {
	int copy[24];
	for (int i = 0; i < 24; i++) copy[i] = pos[i];

	bool used[16] = {false};
	std::vector<DupGroup> groups;
	std::vector<OpenSlot> openSlots; // partial + duplicated open slots

	for (int i = 0; i < 24; i++) {
		int v = pos[i];
		if (v >= 0 && v <= 15) {
			used[v] = true;
			if (v < 8) i++;
			continue;
		}
		if (v <= -1000 || v >= 1000) {
			int p = v <= -1000 ? (-1000 - v) : (v - 1000);
			bool found = false;
			for (auto &g : groups) if (g.piece == p) { g.members.push_back(i); found = true; break; }
			if (!found) groups.push_back({p, {i}});
			if (p < 8) i++;
			continue;
		}
		// a normal partial piece
		bool corner = v < 0;
		int layer = corner ? (v % 3 == 0 ? 0 : v % 3 == -2 ? 1 : 2)
		                    : (v % 3 == 0 ? 0 : v % 3 == 1 ? 1 : 2);
		openSlots.push_back({i, corner, layer});
		if (corner) i++;
	}
	for (auto &g : groups) used[g.piece] = true; // reserve concrete/duplicated pieces

	// Remaining candidates for both piece types, consumed/restored by the search.
	std::vector<int> cornerPool, edgePool;
	for (int p = 0; p < 8;  p++) if (!used[p]) cornerPool.push_back(p);
	for (int p = 8; p < 16; p++) if (!used[p]) edgePool.push_back(p);

	auto layerOk = [](int layer, bool corner, int p) {
		if (layer == 2) return true;
		if (corner) return layer == 0 ? (p < 4) : (p >= 4);
		return layer == 0 ? (p < 12) : (p >= 12);
	};

	// Assign every open slot one at a time, modifying the shared corner/edge pools as we go.
	std::function<bool(size_t)> assign = [&](size_t idx) -> bool {
		if (idx == openSlots.size()) return visit(copy); // once everything assigned, call visit()
		auto &slot = openSlots[idx];
		auto &pool = slot.corner ? cornerPool : edgePool;
		for (size_t k = 0; k < pool.size(); k++) {
			int p = pool[k];
			if (!layerOk(slot.layer, slot.corner, p)) continue;
			pool.erase(pool.begin() + k);
			copy[slot.index] = p;
			if (slot.corner) copy[slot.index + 1] = p;
			bool stop = assign(idx + 1);
			pool.insert(pool.begin() + k, p);
			if (stop) return true;
		}
		return false;
	};

	// for each duplicate group, pin the piece to any of the slots (the rest become W/Z pieces)
	std::function<bool(size_t)> pin = [&](size_t gi) -> bool {
		if (gi == groups.size()) return assign(0); // once everything pinned, assign()
		auto &g = groups[gi];
		bool corner = g.piece < 8;
		for (size_t chosen = 0; chosen < g.members.size(); chosen++) {
			size_t before = openSlots.size();
			for (size_t m = 0; m < g.members.size(); m++) {
				int idx = g.members[m];
				if (m == chosen) {
					copy[idx] = g.piece;
					if (corner) copy[idx + 1] = g.piece;
				} else {
					openSlots.push_back({idx, corner, 2});
				}
			}
			bool stop = pin(gi + 1);
			openSlots.resize(before);
			if (stop) return true;
		}
		return false;
	};

	return pin(0);
}

// is ANY completion 2g?
bool cornersAre2GenSolvableExact(const int pos[24], int twoGen, bool specificAngleBot = false) {
	if (twoGen == 0) return true;
	return forEachCompletion(pos, [&](const int completed[24]) {
		return cornersAre2GenSolvable(completed, twoGen, specificAngleBot);
	});
}

/**
 * Union of every valid preADF across all completions.
 * Keeps searching until either completions or preADFs are exhausted.
 */
std::vector<int> twoGenPreADFExact(const int pos[24], int twoGen, bool specificAngleBot = false) {
	if (twoGen == 0) return {0};
	bool found[12] = {false};
	int total = 0;
	forEachCompletion(pos, [&](const int completed[24]) {
		for (int k : twoGenPreADF(completed, twoGen, specificAngleBot)) {
			if (!found[k]) { found[k] = true; total++; }
		}
		return total >= 12;
	});
	std::vector<int> result;
	for (int k = 0; k < 12; k++) if (found[k]) result.push_back(k);
	return result;
}

}


// Tracks a group where the same concrete piece was declared multiple times (duplicated).
struct DuplicateSpec {
	int pieceValue;    // 0-7 for corners, 8-15 for edges
	int solvedPosIdx;  // pos[] index of this piece in solved state
	int count;         // how many times it appeared
	int baseValue;     // base piece value for this group (unique)
	int values[8];     // the actual values assigned (up to 8)
};

/*
 * Piece numbers below 0 are partially specified corners.
 * Based on the value modulo 3, it's a top corner (0), bottom corner (-2), or any corner (-1).
 * Piece numbers above 15 are partially specified edges.
 * Based on the value modulo 3, it's top edge (0), bottom edge (1), or any edge (2).
 */
class FullPosition {
public:
	int pos[24];
	int middle;
	std::vector<DuplicateSpec> duplicates;
	char inputChars[16]; // original input characters for print()
	FullPosition(){ reset(); }
	void reset(){
		middle=1;
		duplicates.clear();
		for( int i=0; i<24; i++)
			pos[i]="AAIBBJCCKDDLMEENFFOGGPHH"[i]-'A';
	}
	void print(){
		// maps pos[] index -> input string position (0-15)
		static const int posToInput[24] = {0,0,1,2,2,3,4,4,5,6,6,7,8,9,9,10,11,11,12,13,13,14,15,15};
		for(int i=0; i<24; i++){
			if (pos[i] > 43) {
				// duplicate edge
				std::cout<<inputChars[posToInput[i]];
			} else if (pos[i] > 15) {
				std::cout<<"XYZ"[pos[i]%3];
			} else if (pos[i] < 0 && pos[i] <= -25) {
				// duplicate corner
				std::cout<<inputChars[posToInput[i]];
			} else if (pos[i] < 0) {
				std::cout<<"UWV"[(-pos[i])%3];
			} else {
				std::cout<<"ABCDEFGH12345678"[pos[i]];
			}
			if( pos[i]<8 ) i++;
		}
		std::cout<<"/ -"[middle+1];
	}
	void random(int twoGen, bool keepCubeShape){
		middle = (rand()&1)!=0?-1:1;
		do{
			// make starting position
			int tmp[16];
			for( int i=0; i<8; i++) {
				tmp[2*i + (i>3?1:0)] = i;
				tmp[2*i + (i>3?0:1)] = 8+i;
			}
			// shuffle
			if (keepCubeShape) {
				bool parity = false;
				int cornersToMix = twoGen==1 ? 6 : 8;
				int edgesToMix = twoGen==1 ? 7 : 8;
				for (int i=0; i<cornersToMix; i++) {
					int j = i + rand() % (cornersToMix - i);
					int k = tmp[2*i + (i>3?1:0)];
					tmp[2*i + (i>3?1:0)] = tmp[2*j + (j>3?1:0)];
					tmp[2*j + (j>3?1:0)] = k;
					if (i!=j) parity ^= true;
				}
				for (int i=0; i<edgesToMix; i++) {
					int j = i + rand() % (edgesToMix - i);
					int k = tmp[2*i + (i>3?0:1)];
					tmp[2*i + (i>3?0:1)] = tmp[2*j + (j>3?0:1)];
					tmp[2*j + (j>3?0:1)] = k;
					if (i!=j) parity ^= true;
				}
				if (parity) {
					int k = tmp[0];
					tmp[0] = tmp[2];
					tmp[2] = k;
				}
			} else {
				int nToMix = twoGen==2 ? 12 : (twoGen==1 ? 13 : 16);
				for( int i=0;i<nToMix; i++){
					int j=rand()%(nToMix-i);
					int k=tmp[i];tmp[i]=tmp[i+j];tmp[i+j]=k;
				}
			}
			// convert to position array
			for(int i=0, j=0;i<16;i++){
				pos[j++]=tmp[i];
				if( tmp[i]<8 ) pos[j++]=tmp[i];
			}
			// if p2g and keeping cubeshape, are the corners solvable in 2gen? if not, try again
			if (twoGen == 1 && keepCubeShape) {
				if (!has2GenCorners()) {
					pos[6] = pos[5]; continue; // fail the condition
				}
			}
			// ABF. if keeping cube shape, adjust both; otherwise, if p2g, adjust D only
			if (keepCubeShape) {
				if ((rand()&1)!=0) {
					tmp[0] = pos[11];
					for (int i=10; i>=0; i--) {
						pos[i+1] = pos[i];
					}
					pos[0] = tmp[0];
				}
				if ((rand()&1)!=0) {
					tmp[0] = pos[12];
					for (int i=12; i<=22; i++) {
						pos[i] = pos[i+1];
					}
					pos[23] = tmp[0];
				}
			} else if (twoGen == 1 && (rand()&1)!=0 && pos[11]!=pos[12]) {
				// in pseudo 2gen, with 50% chance, if the layers are validly separated, do a (0,-1)
				tmp[0] = pos[12];
				for (int i=12; i<=22; i++) {
					pos[i] = pos[i+1];
				}
				pos[23] = tmp[0];
			}
			// test sliceable
		}while( pos[5]==pos[6] || pos[11]==pos[12] || pos[17]==pos[18] || pos[12]==pos[23]);
	}
	void set(int p[],int m){
		for(int i=0;i<24;i++)pos[i]=p[i];
		middle=m;
	};
	void doTop(int m){
		m%=12;
		if(m<0)m+=12;
		while(m>0){
			int c=pos[11];
			for(int i=11;i>0;i--) pos[i]=pos[i-1];
			pos[0]=c;
			m--;
		}
	}
	void doBot(int m){
		m%=12;
		if(m<0)m+=12;
		while(m>0){
			int c=pos[23];
			for(int i=23;i>12;i--) pos[i]=pos[i-1];
			pos[12]=c;
			m--;
		}
	}
	bool doSlice(){
		if( !isSliceable() ) return false;
		for(int i=6;i<12;i++){
			int c=pos[i];
			pos[i]=pos[i+6];
			pos[i+6]=c;
		}
		middle=-middle;
		return true;
	}
	bool isSliceable(){
		return( pos[0]!=pos[11] && pos[5]!=pos[6] && pos[12]!=pos[23] && pos[17]!=pos[18] );
	}
	int getShape(){
		int s=0;
		for(int m=1<<23,i=0; i<24; i++,m>>=1){
			if(pos[i]>=8) s|=m;
		}
		return(s);
	}
	bool getParityOdd(){
		bool p=false;
		for(int i=0; i<24; i++){
			for(int j=i; j<24; j++){
				if( pos[j]<pos[i]) p=!p;
				if(pos[j]<8)j++;
			}
			if(pos[i]<8)i++;
		}
		return(p);
	}
	int getEdgeColoring(int cl){
		const int clp[3][4]={ { 8, 9,10,11}, { 8, 9,13,14}, {15,14,10, 9} };
		int c=0;
		int cnt=0;
		int m=(cl!=2)?1<<7:1;
		for(int i=0; i<24; i++){
			if( pos[i]>=8 ){
				for(int j=0; j<4; j++){
					if( pos[i]==clp[cl][j] || (pos[i]>15 && pos[i]%3==0 && cl==0)) { // edge up
						c|=m;
						cnt++;
						break;
					}
				}
				if(cl!=2) m>>=1; else m<<=1;
			}
		}
		if (cnt==4) return c;
		else return -1;
	}
	int getCornerColoring(int cl){
		const int clp[3][4]={ {0,1,2,3}, {0,1,5,6}, {7,6,2,1} };
		int c=0;
		int cnt=0;
		int m=(cl!=2)?1<<7:1;
		for(int i=0; i<24; i++){
			if( pos[i]<8 ){
				for(int j=0; j<4; j++){
					if( pos[i]==clp[cl][j] || (pos[i]<0 && pos[i]%3==0 && cl==0)) { // corner up
						c|=m;
						cnt++;
						break;
					}
				}
				if(cl!=2) m>>=1; else m<<=1;
				i++;
			}
		}
		if (cnt==4) return c;
		else return -1;
	}
	bool parseNumberForward(const char*inp, int& ix, int& num){
		bool min = false;
		num = 0;
		while( inp[ix]==' ' ) ix++;
		if( inp[ix]=='-') {
			min=true;
			ix++;
		}
		if( inp[ix]<'0' || inp[ix]>'9' ) return true;
		while( inp[ix]>='0' && inp[ix]<='9' ){
			num =num*10+(inp[ix]-'0');
			ix++;
		}
		if( min ) num = -num;
		while( inp[ix]==' ' ) ix++;
		return false;
	}
	bool parseNumberBackward(const char*inp, int& ix, int& num){
		int digvalue = 1;
		num = 0;
		while( ix>=0 && inp[ix]==' ' ) ix--;
		if( ix<0 ) return true;
		if( inp[ix]<'0' || inp[ix]>'9' ) return true;
		while( ix>=0 && inp[ix]>='0' && inp[ix]<='9' ){
			num =num+digvalue*(inp[ix]-'0');
			digvalue*=10;
			ix--;
		}
		if( ix>=0 && inp[ix]=='-'){
			num = -num;
			ix--;
		}
		while( ix>=0 && inp[ix]==' ' ) ix++;
		return false;
	}
	int parseInput( const char* inp ){
		// scan characters
		const char* t=inp;
		int f=0;
		while(*t){
			if( *t == ',' || *t == '(' || *t == ')' || *t == '9' || *t == '0' ){
				f|=1; // cannot be position string, but may be movelist
			}else if( (*t>='a' && *t<='h') || (*t>='A' && *t<='H') || (*t>='u' && *t<='z') || (*t>='U' && *t<='Z') ){
				f|=2; // cannot be movelist, but may be position string
			}else if( *t!='/' && *t!='-' && (*t<'1' || *t>'8') ){
				f|=3; // cannot be either
			}
			t++;
		}
		if( f==3 || f==0 ){
			return(13);
		}

		reset();
		int lw=0,lu=0;
		if( f==1 && !generator){
			// solution move sequence. start parsing from end
			int md=0;
			int i=strlen(inp)-1;
			while( i>=0 ){
				while( i>=0 && inp[i]==' ' ) i--;
				if( md==0 ){   // parsing any move
					if(inp[i]=='/') md = 1;
					else md = 2;
				}else if( md==1 ){
					if(inp[i--]!='/') return 16;
					if(!doSlice()) return 12;
					lu++;lw++;
					md=2;
				}else if( md==2 ){
					int m = 0;
					bool br=false;
					if( inp[i]==')' ) { i--; br=true; }
					// parsing bot turn
					if( parseNumberBackward(inp, i, m) ) return 5;
					m%=12;
					doBot(-m);
					if(m!=0) lu++;
					if( i<0 || inp[i--]!=',' ) return 6;
					// parsing top turn
					if( parseNumberBackward(inp, i, m) ) return 7;
					m%=12;
					doTop(-m);
					if(m!=0) lu++;
					if( br && ( i<0 || inp[i--]!='(' )) return 8;
					md--;
				}
			}
			if( !isSliceable() ) return 12;
			if( verbosity>=2) std::cout<<"Input:"<<inp<<" ["<<lw<<"|"<<lu<<"]"<<std::endl;
		}else if( f==1 ){
			// generating move sequence. start parsing from beginning
			int md=0;
			int i=0;
			while( inp[i]!=0 ){
				while( inp[i]==' ' ) i++;
				if( md==0 ){   // parsing any move
					if(inp[i]=='/') md = 1;
					else md = 2;
				}else if( md==1 ){
					if(inp[i++]!='/') return 16;
					if(!doSlice()) return 12;
					lu++;lw++;
					md=2;
				}else if( md==2 ){
					int m = 0;
					bool br=false;
					if( inp[i]=='(' ) { i++; br=true; }
					// parsing top turn
					if( parseNumberForward(inp, i, m) ) return 7;
					m%=12;
					doTop(m);
					if(m!=0) lu++;
					if( inp[i++]!=',' ) return 6;
					// parsing bot turn
					if( parseNumberForward(inp, i, m) ) return 5;
					m%=12;
					doBot(m);
					if(m!=0) lu++;
					if( br && inp[i++]!=')' ) return 4;
					md--;
				}
			}
			if( !isSliceable() ) return 12;
			if( verbosity>=2) std::cout<<"Input:"<<inp<<" ["<<lw<<"|"<<lu<<"]"<<std::endl;
		}else{
			// position
			if( strlen(inp)!=16 && strlen(inp)!=17 ) return(9);
			int pieceCount[16]; // track counts of each piece
			int cecount[6]; // track total [up, down, all] + 3*[corners, edges]
			for (int i=0; i<16; i++) pieceCount[i] = 0;
			for (int i=0; i<6; i++) cecount[i] = 0;

			// store original input characters for duplicate pieces print-back
			for (int i=0; i<16; i++) {
				char c = inp[i];
				if (c >= 'a' && c <= 'z') c += 'A'-'a';
				inputChars[i] = c;
			}

			// first pass: count concrete pieces to detect duplicates
			for( int i=0; i<16; i++){
				int k=inp[i];
				if(k>='a' && k<='z') k+=('A'-'a');
				if(k>='A' && k<='H') pieceCount[k-'A']++;
				else if(k>='1' && k<='8') pieceCount[k-'1'+8]++;
			}

			// set up duplicate piece value ranges for duplicate groups
			// corner groups: base = -25, -49, -73, ... (spacing 24)
			//   values go DOWN: base, base-3, base-6, ... (all mod 3 == -1, like W)
			// edge groups:   base = 44, 68, 92, ...   (spacing 24)
			//   values go UP: base, base+3, base+6, ... (all mod 3 == 2, like Z)
			int nextTCornerGroup = -25;
			int nextSEdgeGroup = 44;
			duplicates.clear();

			int j=0;
			int pi[24];
			// we can't reuse a piece number because two of the same number means a corner, so
			// each partially defined piece gets a separate set of 3 possible values
			int nextPartialCorner = -3;
			int nextPartialEdge = 18;

			// solved pos[] index for each concrete piece (A=0->pos0, B=1->pos3, etc.)
			const int solvedPosIdx[16] = {0, 3, 6, 9, 13, 16, 19, 22, 2, 5, 8, 11, 12, 15, 18, 21};

			for( int i=0; i<16; i++){
				int k=inp[i];
				if(k>='a' && k<='z') k+=('A'-'a');
				if(k>='A' && k<='H') {
					int pidx = k-'A';
					if (pieceCount[pidx] > 1) {
						// duplicate corner: check if we already have a group for this piece
						int groupBase = 0;
						bool found = false;
						for (size_t g=0; g<duplicates.size(); g++) {
							if (duplicates[g].pieceValue == pidx) {
								groupBase = duplicates[g].baseValue;
								found = true;
								break;
							}
						}
						if (!found) {
							groupBase = nextTCornerGroup;
							nextTCornerGroup -= 24;
							DuplicateSpec ds;
							ds.pieceValue = pidx;
							ds.solvedPosIdx = solvedPosIdx[pidx];
							ds.count = 0;
							ds.baseValue = groupBase;
							memset(ds.values, 0, sizeof(ds.values));
							duplicates.push_back(ds);
						}
						// find group and assign duplicate value: base, base-3, base-6, ...
						for (size_t g=0; g<duplicates.size(); g++) {
							if (duplicates[g].pieceValue == pidx) {
								k = duplicates[g].baseValue - 3*duplicates[g].count;
								duplicates[g].values[duplicates[g].count] = k;
								duplicates[g].count++;
								break;
							}
						}
					} else {
						// single corner: concrete value
						k = pidx;
					}
				}
				else if(k>='1' && k<='8') {
					int pidx = k-'1'+8;
					if (pieceCount[pidx] > 1) {
						// duplicate edge
						int groupBase = 0;
						bool found = false;
						for (size_t g=0; g<duplicates.size(); g++) {
							if (duplicates[g].pieceValue == pidx) {
								groupBase = duplicates[g].baseValue;
								found = true;
								break;
							}
						}
						if (!found) {
							groupBase = nextSEdgeGroup;
							nextSEdgeGroup += 24;
							DuplicateSpec ds;
							ds.pieceValue = pidx;
							ds.solvedPosIdx = solvedPosIdx[pidx];
							ds.count = 0;
							ds.baseValue = groupBase;
							memset(ds.values, 0, sizeof(ds.values));
							duplicates.push_back(ds);
						}
						// find group and assign duplicate value: base, base+3, base+6, ...
						for (size_t g=0; g<duplicates.size(); g++) {
							if (duplicates[g].pieceValue == pidx) {
								k = duplicates[g].baseValue + 3*duplicates[g].count;
								duplicates[g].values[duplicates[g].count] = k;
								duplicates[g].count++;
								break;
							}
						}
					} else {
						k = pidx;
					}
				}
				else if(k>='U' && k<='W') {
					k+=(nextPartialCorner-'U');
					nextPartialCorner -= 3;
				}
				else if(k>='X' && k<='Z') {
					k+=(nextPartialEdge-'X');
					nextPartialEdge += 3;
				}
				else return(10);
				pi[j++] = k;
				if (k<8) {
					pi[j++] = k;
					cecount[2]++;
					if ((k<0 && k%3==0) || (k>=0 && k<=3)) cecount[0]++; // corner up
					if ((k<0 && k%3==-2) || (k>=4 && k<=7)) cecount[1]++; // corner down
				} else {
					cecount[5]++;
					if ((k>15 && k%3==0) || (k>=8 && k<=11)) cecount[3]++; // edge up
					if ((k>15 && k%3==1) || (k>=12 && k<=15)) cecount[4]++; // edge down
				}
			}
			if (cecount[0] > 4 || cecount[1] > 4 || cecount[2] > 8 || cecount[3] > 4 || cecount[4] > 4 || cecount[5] > 8) return 17;
			int midLayer=0;
			if( strlen(inp)==17 ){
				int k=inp[16];
				if( k!='-' && k!='/' ) return(11);
				midLayer = (k=='-') ? 1 : -1;
			}
			set(pi,midLayer);
		}
		return(0);
	}
	// assuming we're in a squares, check if the corners are solvable with 2gen
	bool has2GenCorners(){ return ::has2GenCorners(pos); }
	// valid preADF for 2g/p2g
	std::vector<int> findPreADF(int twoGen) const { return twoGenPreADF(pos, twoGen, specificAngleBot, false); }
	bool singleMatch(int posI, int solvedI) { return couldBe(posI, solvedI); }
	bool matchesSolved() {
		int solved[24] = {0, 0, 8, 1, 1, 9, 2, 2, 10, 3, 3, 11, 12, 4, 4, 13, 5, 5, 14, 6, 6, 15, 7, 7};
		for (int i=0; i<24; i++) {
			if (!singleMatch(pos[i], solved[i])) return false;
		}
		return true;
	}
	bool isPartial() {
		for (int i=0; i<24; i++) {
			if (pos[i] < 0 || pos[i] > 15) return true;
		}
		return false;
	}

};

// "is this good squares?"
static inline bool isCubeShape(int shp) {
	return shp==5052 || shp==4148 || shp==5039 || shp==4163;
}

// used to map cubeshape indices to `table` indices in CubePrunTable
static const int CUBE_LOCAL2RAW[4] = {4148, 4163, 5039, 5052};
static inline int cubeRaw2Local(int raw) {
	for (int i=0; i<4; i++) if (CUBE_LOCAL2RAW[i]==raw) return i;
	return -1;
}

// Pruning table restricted to cubeshape only.
class CubePrunTable {
public:
	char (*table)[70][70]; // [4][70][70]
	ShapeTranTable& stt;
	ShpColTranTable& scte;
	ShpColTranTable& sctc;

	CubePrunTable( FullPosition& p0, int cl, ShapeTranTable& stt0, ShpColTranTable& scte0, ShpColTranTable& sctc0)
		: stt(stt0), scte(scte0), sctc(sctc0)
	{
		table = new char[4][70][70];
		std::string fname;
		if (metric == TURN_METRIC) {
			fname = tablePath((cl==0) ? "sq1cp1u.dat" : "sq1cp2u.dat");
		} else if (metric == ANGLE_METRIC) {
			fname = tablePath((cl==0) ? "sq1cp1a.dat" : "sq1cp2a.dat");
		} else {
			fname = tablePath((cl==0) ? "sq1cp1w.dat" : "sq1cp2w.dat");
		}

		std::ifstream is( fname, std::ios::binary );
		if( is.fail() ){
			for(int i0=0;i0<4;i0++) for(int i1=0;i1<70;i1++) for(int i2=0;i2<70;i2++) table[i0][i1][i2]=0;

			int s0raw = stt.getShape(p0.getShape(), p0.getParityOdd());
			int s0 = cubeRaw2Local(s0raw);
			if( s0>=0 ){
				int e0 = p0.getEdgeColoring(cl);
				int c0 = p0.getCornerColoring(cl);
				e0 = scte0.ct.choice2Idx[e0];
				c0 = sctc0.ct.choice2Idx[c0];
				if (metric == TURN_METRIC || metric == ANGLE_METRIC) {
					table[s0][e0][c0] = 1;
				} else {
					setAll(s0raw, s0, e0, c0, 1);
				}

				char l=1;
				int n=1;
				int last_nonzero=-1;
				do{
					throwIfStopped();
					n=0;
					if (metric == TURN_METRIC) {
						for(int i0=0;i0<4;i0++){
							int rawI0 = CUBE_LOCAL2RAW[i0];
							for(int i1=0;i1<70;i1++){
							for(int i2=0;i2<70;i2++){
								if( table[i0][i1][i2]==l ){
									for(int m=0;m<3;m++){
										int rawJ0=rawI0, j1=i1, j2=i2;
										int w=0;
										do{
											j2=sctc.tranTable[rawJ0][j2][m];
											j1=scte.tranTable[rawJ0][j1][m];
											rawJ0=stt.tranTable[rawJ0][m];
											int locJ0 = cubeRaw2Local(rawJ0);
											if( locJ0<0 ) break;
											if( table[locJ0][j1][j2]==0 ){
												table[locJ0][j1][j2]=l+1;
												n++;
											}
											w++;
											if(w>12) throw std::runtime_error("Invalid restricted pruning table turn cycle.");
										}while(rawJ0!=rawI0 || j1!=i1 || j2!=i2 );
									}
								}
							}}
						}
					}else if (metric == ANGLE_METRIC) {
						for(int i0=0;i0<4;i0++){
							int rawI0 = CUBE_LOCAL2RAW[i0];
							for(int i1=0;i1<70;i1++){
							for(int i2=0;i2<70;i2++){
								if( table[i0][i1][i2]==l ){
									for(int m=0;m<3;m++){
										int rawJ0=rawI0, j1=i1, j2=i2;
										int w=0, newcnt=0;
										do{
											if(m==0){
												w+=stt.getTopTurn(rawJ0);
											}else if(m==1){
												w+=stt.getBotTurn(rawJ0);
											}else{
												w++;
											}
											j2=sctc.tranTable[rawJ0][j2][m];
											j1=scte.tranTable[rawJ0][j1][m];
											rawJ0=stt.tranTable[rawJ0][m];
											int locJ0 = cubeRaw2Local(rawJ0);
											if( locJ0<0 ) break;
											if (m==2) {
												newcnt = l + 1;
											} else {
												newcnt = l + (w>6 ? 12-w : w);
											}
											if( table[locJ0][j1][j2]==0 || table[locJ0][j1][j2] > newcnt ){
												table[locJ0][j1][j2]=newcnt;
												n++;
											}
											if(w>12) throw std::runtime_error("Invalid restricted pruning table angle cycle.");
										}while(rawJ0!=rawI0 || j1!=i1 || j2!=i2 );
									}
								}
							}}
						}
					}else{
						for(int i0=0;i0<4;i0++){
							int rawI0 = CUBE_LOCAL2RAW[i0];
							for(int i1=0;i1<70;i1++){
							for(int i2=0;i2<70;i2++){
								if( table[i0][i1][i2]==l ){
									int rawSliceTarget = stt.tranTable[rawI0][2];
									int localSliceTarget = cubeRaw2Local(rawSliceTarget);
									if( localSliceTarget>=0 ){
										int j1 = scte.tranTable[rawI0][i1][2];
										int j2 = sctc.tranTable[rawI0][i2][2];
										if( table[localSliceTarget][j1][j2]==0 ){
											n += setAll(rawSliceTarget, localSliceTarget, j1, j2, l+1);
										}
									}
								}
							}}
						}
					}
					l++;
					if(n!=0) last_nonzero=l;
				}while(l - last_nonzero < 10);
			}

			std::ofstream os( fname, std::ios::binary );
			os.write( (char*)table, 4*70*70*sizeof(char) );
		}else{
			is.read( (char*)table, 4*70*70*sizeof(char) );
		}
	}
	~CubePrunTable(){ delete[] table; }

	// Set a position to depth l, as well as all ADF of it, slice-metric only.
	// (mirrors PrunTable::setAll)
	inline int setAll(int rawI0, int locI0, int i1, int i2, char l){
		int n=0;
		int rawJ0=rawI0, j1=i1, j2=i2;
		do{
			int rawK0=rawJ0, k1=j1, k2=j2;
			do{
				int locK0 = cubeRaw2Local(rawK0);
				if( table[locK0][k1][k2]==0 ){
					table[locK0][k1][k2]=l;
					n++;
				}
				k2=sctc.tranTable[rawK0][k2][0];
				k1=scte.tranTable[rawK0][k1][0];
				rawK0=stt.tranTable[rawK0][0];
			}while(rawK0!=rawJ0 || k1!=j1 || k2!=j2 );
			j2=sctc.tranTable[rawJ0][j2][1];
			j1=scte.tranTable[rawJ0][j1][1];
			rawJ0=stt.tranTable[rawJ0][1];
		}while(rawJ0!=rawI0 || j1!=i1 || j2!=i2 );
		return n;
	}
};

/**
 * color index (0..69, or -1) of any 4 pieces (of the same type).
 * (generalized FullPosition::getCornerColoring/getEdgeColoring)
 * Only concrete pieces will be chosen to be marked in pos[].
 */
static inline int markedColorIdx(const int pos[24], const int marked[4], bool isEdge, ChoiceTable& ct){
	int c=0, cnt=0;
	int m=1<<7;
	for(int i=0; i<24; i++){
		bool thisType = isEdge ? (pos[i]>=8) : (pos[i]<8);
		if(thisType){
			for(int j=0;j<4;j++){
				if(pos[i]==marked[j]){ c|=m; cnt++; break; }
			}
			m>>=1;
			if(!isEdge) i++;
		}
	}
	return (cnt==4) ? ct.choice2Idx[c] : -1;
}

/**
 * Dynamic corner-only/edge-only CS-restricted pruning tables for partial solving.
 *
 * using ShapeColPos/ShpColTranTable (sctc/scte):
 * (a) compute the color index for an arbitrary set of corners/edges (markedColorIdx)
 * (b) a small BFS flood fill seeded at that set's solved index
 *
 * NOT stored to disk.
 */
class DynCubePrunTable1D {
public:
	char table[4][70];
	bool valid=false;

	// sc is sctc for corners, scte for edges.
	DynCubePrunTable1D(const int marked[4], bool isEdge, ShapeTranTable& stt, ShpColTranTable& sc, ChoiceTable& ct){
		for(int i0=0;i0<4;i0++) for(int i1=0;i1<70;i1++) table[i0][i1]=0;

		FullPosition q; // solved position
		int s0raw = stt.getShape(q.getShape(), q.getParityOdd());
		int s0 = cubeRaw2Local(s0raw);
		if (s0 < 0) return; // just so the universe doesn't play a trick on us
		int c0 = markedColorIdx(q.pos, marked, isEdge, ct);
		if (c0 < 0) return; // reject marked[] if it's not all concrete and valid

		if (metric == TURN_METRIC || metric == ANGLE_METRIC) {
			table[s0][c0] = 1;
		} else {
			setAll(s0raw, s0, c0, 1, stt, sc);
		}

		char l=1;
		int n=1;
		int last_nonzero=-1;
		do{
			throwIfStopped();
			n=0;
			if (metric == TURN_METRIC) {
				for(int i0=0;i0<4;i0++){
					int rawI0 = CUBE_LOCAL2RAW[i0];
					for(int i1=0;i1<70;i1++){
						if( table[i0][i1]==l ){
							for(int m=0;m<3;m++){
								int rawJ0=rawI0, j1=i1;
								int w=0;
								do{
									j1=sc.tranTable[rawJ0][j1][m];
									rawJ0=stt.tranTable[rawJ0][m];
									int locJ0 = cubeRaw2Local(rawJ0);
									if( locJ0<0 ) break;
									if( table[locJ0][j1]==0 ){
										table[locJ0][j1]=l+1;
										n++;
									}
									w++;
									if(w>12) throw std::runtime_error("Invalid dynamic pruning table turn cycle.");
								}while(rawJ0!=rawI0 || j1!=i1);
							}
						}
					}
				}
			}else if (metric == ANGLE_METRIC) {
				for(int i0=0;i0<4;i0++){
					int rawI0 = CUBE_LOCAL2RAW[i0];
					for(int i1=0;i1<70;i1++){
						if( table[i0][i1]==l ){
							for(int m=0;m<3;m++){
								int rawJ0=rawI0, j1=i1;
								int w=0, newcnt=0;
								do{
									if(m==0) w+=stt.getTopTurn(rawJ0);
									else if(m==1) w+=stt.getBotTurn(rawJ0);
									else w++;
									j1=sc.tranTable[rawJ0][j1][m];
									rawJ0=stt.tranTable[rawJ0][m];
									int locJ0 = cubeRaw2Local(rawJ0);
									if( locJ0<0 ) break;
									if (m==2) newcnt = l+1;
									else newcnt = l + (w>6 ? 12-w : w);
									if( table[locJ0][j1]==0 || table[locJ0][j1] > newcnt ){
										table[locJ0][j1]=newcnt;
										n++;
									}
									if(w>12) throw std::runtime_error("Invalid dynamic pruning table angle cycle.");
								}while(rawJ0!=rawI0 || j1!=i1);
							}
						}
					}
				}
			}else{ // SLICE_METRIC
				for(int i0=0;i0<4;i0++){
					int rawI0 = CUBE_LOCAL2RAW[i0];
					for(int i1=0;i1<70;i1++){
						if( table[i0][i1]==l ){
							int rawSliceTarget = stt.tranTable[rawI0][2];
							int localSliceTarget = cubeRaw2Local(rawSliceTarget);
							if( localSliceTarget>=0 ){
								int j1 = sc.tranTable[rawI0][i1][2];
								if( table[localSliceTarget][j1]==0 ){
									n += setAll(rawSliceTarget, localSliceTarget, j1, l+1, stt, sc);
								}
							}
						}
					}
				}
			}
			l++;
			if(n!=0) last_nonzero=l;
		}while(l - last_nonzero < 10);

		valid = true;
	}

private:
	// CubePrunTable::setAll but single-axis. Slice-metric only.
	int setAll(int rawI0, int /*locI0*/, int i1, char l, ShapeTranTable& stt, ShpColTranTable& sc){
		int n=0;
		int rawJ0=rawI0, j1=i1;
		do{
			int rawK0=rawJ0, k1=j1;
			do{
				int locK0 = cubeRaw2Local(rawK0);
				if( table[locK0][k1]==0 ){
					table[locK0][k1]=l;
					n++;
				}
				k1=sc.tranTable[rawK0][k1][0];
				rawK0=stt.tranTable[rawK0][0];
			}while(rawK0!=rawJ0 || k1!=j1);
			j1=sc.tranTable[rawJ0][j1][1];
			rawJ0=stt.tranTable[rawJ0][1];
		}while(rawJ0!=rawI0 || j1!=i1);
		return n;
	}
};

/**
 * LRU cache of built DynCubePrunTable1D tables, for reuse.
 *
 * Tables can be identified by:
 * 1. its marked 4-piece set
 * 2. corners or edges
 * (3. metric, but this is a session constant, set once by batchParseFlags)
 */
class DynTableCache {
public:
	struct Key {
		uint64_t a, b;   // packed piece ids (0-15) into two halves of a bitmask
		bool isEdge;
		bool operator==(const Key& o) const { return a==o.a && b==o.b && isEdge==o.isEdge; }
		Key(const std::array<int,4>& m, bool edge): a(0), b(0), isEdge(edge){
			for(int v : m){ uint64_t bit = (uint64_t)1 << (uint64_t)(v & 63); if(v<64) a|=bit; else b|=bit; }
		}
	};
	struct KeyHash {
		size_t operator()(const Key& k) const {
			size_t h = std::hash<uint64_t>()(k.a);
			h ^= std::hash<uint64_t>()(k.b) + 0x9e3779b9 + (h<<6) + (h>>2);
			return h ^ (k.isEdge ? 0xabcd1234 : 0);
		}
	};

	explicit DynTableCache(size_t capacity) : _cap(capacity>0?capacity:32) {}

	/**
	 * Given `marked`/`isEdge`, returns a pointer to a built table (build if nonexistent).
	 * Callers must consume the returned pointer immediately since it's not valid forever.
	 * 		(see buildDynamicTables).
	 */
	const DynCubePrunTable1D* get(const std::array<int,4>& marked, bool isEdge,
	                              ShapeTranTable& stt, ShpColTranTable& sc, ChoiceTable& ct){
		Key key(marked, isEdge);
		auto it = _map.find(key);
		if (it != _map.end()){
			// hit. move to most-recently-used
			_ring.erase(_ring.find(it->second));
			int slot = it->second;
			_ring[slot] = _clock++;
			return &_vec[slot];
		}
		// build
		int slot;
		if (_vec.size() < _cap){
			slot = (int)_vec.size();
			_vec.emplace_back(marked.data(), isEdge, stt, sc, ct);
		} else {
			// evict least-recently-used
			slot = _ring.begin()->first;
			// drop the evicted slot's old key mapping
			for (auto it = _map.begin(); it != _map.end(); ++it)
				if (it->second == slot){ _map.erase(it); break; }
			_ring.erase(_ring.begin());
			_vec[slot] = DynCubePrunTable1D(marked.data(), isEdge, stt, sc, ct);
		}
		_map[key] = slot;
		_ring[slot] = _clock++;
		return &_vec[slot];
	}

	void clear(){ _vec.clear(); _map.clear(); _ring.clear(); _clock=0; }

private:
	size_t _cap;
	uint64_t _clock = 0;
	std::vector<DynCubePrunTable1D> _vec;
	std::unordered_map<Key, int, KeyHash> _map;
	std::map<int,uint64_t> _ring; // slot -> last used clock
};

// Enumerate every 4-element combination of `items`, capped at C(8,4)=70.
static inline std::vector<std::array<int,4>> chooseFour(const std::vector<int>& items){
	std::vector<std::array<int,4>> out;
	const size_t MAX_COMBOS = 70;
	int n = (int)items.size();
	for(int a=0; a<n && out.size()<MAX_COMBOS; a++)
	for(int b=a+1; b<n && out.size()<MAX_COMBOS; b++)
	for(int c=b+1; c<n && out.size()<MAX_COMBOS; c++)
	for(int d=c+1; d<n && out.size()<MAX_COMBOS; d++)
		out.push_back({items[a], items[b], items[c], items[d]});
	return out;
}

// pruning table for combination of shape, edgeColoring, cornerColoring.
class PrunTable {
public:
	char (*table)[70][70];
	ShapeTranTable& stt;
	ShpColTranTable& scte;
	ShpColTranTable& sctc;

	PrunTable( FullPosition& p0, int cl, ShapeTranTable& stt0, ShpColTranTable& scte0, ShpColTranTable& sctc0)
		: stt(stt0), scte(scte0), sctc(sctc0)
	{
		// Calculate pruning table
		table = new char[NUMSHAPES][70][70];
		std::string fname;
		if(metric == TURN_METRIC){
			fname = tablePath((cl==0)? FILEP1U : FILEP2U);
		} else if (metric == ANGLE_METRIC) {
			fname = tablePath((cl==0)? FILEP1A : FILEP2A);
		} else {
			fname = tablePath((cl==0)? FILEP1W : FILEP2W);
		}

		// see if can be found on file
		std::ifstream is( fname, std::ios::binary );
		if( is.fail() ){
			// no file. calculate table.
			// clear table
			for( int i0=0; i0<NUMSHAPES; i0++ ){
			for( int i1=0; i1<70; i1++){
			for( int i2=0; i2<70; i2++){
				table[i0][i1][i2]=0;
			}}}
			// set start position
			int s0 = stt.getShape(p0.getShape(),p0.getParityOdd());
			int e0 = p0.getEdgeColoring(cl);
			int c0 = p0.getCornerColoring(cl);
			e0 = scte0.ct.choice2Idx[e0];
			c0 = sctc0.ct.choice2Idx[c0];
			if (metric == TURN_METRIC || metric == ANGLE_METRIC){
				table[s0][e0][c0]=1;
			}else{
				setAll(s0,e0,c0,1);
			}

			char l=1;
			int n=1;
			int last_nonzero=-1;
			do{
				throwIfStopped();
				if(verbosity>=6) std::cout<<" l="<<(int)(l-1)<<"  n="<<(int)n<<std::endl;
				n=0;
				if (metric == TURN_METRIC){
					for( int i0=0; i0<NUMSHAPES; i0++ ){
					for( int i1=0; i1<70; i1++){
					for( int i2=0; i2<70; i2++){
						if( table[i0][i1][i2]==l ){
							for( int m=0; m<3; m++){
								int j0=i0, j1=i1, j2=i2;
								int w=0;
								do{
									j2=sctc.tranTable[j0][j2][m];
									j1=scte.tranTable[j0][j1][m];
									j0=stt.tranTable[j0][m];
									if( table[j0][j1][j2]==0 ){
										table[j0][j1][j2]=l+1;
										n++;
									}
									w++;
									if(w>12){
										throw std::runtime_error("Invalid pruning table turn cycle.");
									}
								}while(j0!=i0 || j1!=i1 || j2!=i2 );
							}
						}
					}}}
				}else if (metric == ANGLE_METRIC) {
					for( int i0=0; i0<NUMSHAPES; i0++ ){
					for( int i1=0; i1<70; i1++){
					for( int i2=0; i2<70; i2++){
						if( table[i0][i1][i2]==l ){
							for( int m=0; m<3; m++){ // m is the move type, U/D/slice
								int j0=i0, j1=i1, j2=i2;
								int w=0, newcnt=0;
								do{
									if(m==0){
										w+=stt.getTopTurn(j0);
									}else if(m==1){
										w+=stt.getBotTurn(j0);
									}else{
										w++;
									}
									// w is the move amount
									j2=sctc.tranTable[j0][j2][m];
									j1=scte.tranTable[j0][j1][m];
									j0=stt.tranTable[j0][m];
									if (m==2) {
										newcnt = l + 1;
									} else {
										newcnt = l + (w>6 ? 12-w : w);
									}
									if( table[j0][j1][j2]==0 || table[j0][j1][j2] > newcnt ){
										table[j0][j1][j2]=newcnt;
										n++;
									}
									if(w>12){
										throw std::runtime_error("Invalid pruning table angle cycle.");
									}
								}while(j0!=i0 || j1!=i1 || j2!=i2 );
							}
						}
					}}}
				}else{
					for( int i0=0; i0<NUMSHAPES; i0++ ){
					for( int i1=0; i1<70; i1++){
					for( int i2=0; i2<70; i2++){
						if( table[i0][i1][i2]==l ){
							// do slice
							int j0=stt.tranTable[i0][2];
							int j1=scte.tranTable[i0][i1][2];
							int j2=sctc.tranTable[i0][i2][2];
							if( table[j0][j1][j2]==0 ){
								n+=setAll(j0,j1,j2,l+1);
							}
						}
					}}}
				}
				l++;
				if (n!=0) last_nonzero=l;
			}while(l - last_nonzero < 10);
			if(verbosity>=6) std::cout<<std::endl;

		// save to file
			std::ofstream os( fname, std::ios::binary );
			os.write( (char*)table, NUMSHAPES*70*70*sizeof(char) );
		}else{
			// read from file
			is.read( (char*)table, NUMSHAPES*70*70*sizeof(char) );
		}


	}
	~PrunTable(){
		delete[] table;
	}
	// set a position to depth l, as well as all rotations of it.
	inline int setAll(int i0,int i1,int i2, char l){
		int n=0;
		int j0=i0, j1=i1, j2=i2;
		do{
			int k0=j0, k1=j1, k2=j2;
			do{
				if( table[k0][k1][k2]==0 ){
					table[k0][k1][k2]=l;
					n++;
				}
				k2=sctc.tranTable[k0][k2][0];
				k1=scte.tranTable[k0][k1][0];
				k0=stt.tranTable[k0][0];
			}while(j0!=k0 || j1!=k1 || j2!=k2 );
			j2=sctc.tranTable[j0][j2][1];
			j1=scte.tranTable[j0][j1][1];
			j0=stt.tranTable[j0][1];
		}while(j0!=i0 || j1!=i1 || j2!=i2 );
		return n;
	}
};

/*
 * "useless" segment sequences, i.e. identity move sequences
 *
 * moves are packed using base 12. e.g. (1,2) → 1*12+2
 *
 * IMPORTANT: D move raw values are the NEGATION of the printed value. (11 → 1, 2 → -2)
 * 	So converting a move pair back to raw is asymmetric:
 * 	top = praw(printed), bottom = (12 - praw(printed)) % 12,
 * 	where praw maps moves like -1 to 11 (praw(v) = v<0 ? v+12 : v).
 */
template <size_t NWords, size_t NKeys>
static constexpr std::array<uint64_t, NWords> makeKeyBits(const std::array<uint32_t, NKeys>& keys) {
	std::array<uint64_t, NWords> bits{};
	for (uint32_t key : keys) bits[key >> 6] |= uint64_t{1} << (key & 63);
	return bits;
}

template <size_t NWords>
static inline bool keyBitSet(const std::array<uint64_t, NWords>& bits, uint32_t key) {
	return (bits[key >> 6] & (uint64_t{1} << (key & 63))) != 0;
}

static constexpr int USELESS_SEG_PAIR_KEY_SPACE = 12*12*12*12;
static constexpr int USELESS_SEG_TRIPLE_KEY_SPACE = 12*12*12*12*12*12;

static constexpr std::array<uint32_t, 269> g_uselessSegPairKeys = {{
	222, 366, 510, 654, 798, 1086, 1230, 1374,
	1518, 1662, 1806, 2094, 2238, 2526, 2670, 2958,
	3102, 3379, 3390, 3445, 3534, 3678, 3966, 4110,
	4398, 4542, 4830, 4952, 4974, 5018, 5262, 5406,
	5550, 5694, 5838, 5982, 6126, 6270, 6414, 6525,
	6558, 6591, 6702, 6846, 6990, 7278, 7422, 7710,
	7854, 8098, 8142, 8164, 8286, 8574, 8718, 8862,
	9150, 9294, 9582, 9671, 9726, 9737, 10014, 10158,
	10590, 10734, 10878, 11022, 11166, 11233, 11234, 11235,
	11236, 11237, 11239, 11240, 11241, 11242, 11243, 11244,
	11245, 11246, 11247, 11248, 11249, 11250, 11251, 11252,
	11253, 11254, 11255, 11256, 11257, 11258, 11259, 11260,
	11261, 11262, 11263, 11264, 11265, 11266, 11267, 11268,
	11269, 11270, 11271, 11272, 11273, 11274, 11275, 11276,
	11277, 11278, 11279, 11280, 11281, 11282, 11283, 11284,
	11285, 11286, 11287, 11288, 11289, 11290, 11291, 11292,
	11293, 11294, 11295, 11296, 11297, 11298, 11299, 11300,
	11301, 11302, 11303, 11305, 11306, 11307, 11308, 11309,
	11310, 11311, 11312, 11313, 11314, 11315, 11316, 11317,
	11318, 11319, 11320, 11321, 11322, 11323, 11324, 11325,
	11326, 11327, 11328, 11329, 11330, 11331, 11332, 11333,
	11334, 11335, 11336, 11337, 11338, 11339, 11340, 11341,
	11342, 11343, 11344, 11345, 11346, 11347, 11348, 11349,
	11350, 11351, 11352, 11353, 11354, 11355, 11356, 11357,
	11358, 11359, 11360, 11361, 11362, 11363, 11364, 11365,
	11366, 11367, 11368, 11369, 11370, 11371, 11372, 11373,
	11374, 11375, 11454, 11598, 11742, 11886, 12030, 12174,
	12462, 12606, 12883, 12894, 12949, 13038, 13326, 13470,
	13758, 13902, 14046, 14334, 14456, 14478, 14522, 14766,
	14910, 15198, 15342, 15630, 15774, 15918, 16029, 16062,
	16095, 16206, 16350, 16494, 16638, 16782, 16926, 17070,
	17214, 17358, 17602, 17646, 17668, 17790, 18078, 18222,
	18510, 18654, 18942, 19086, 19175, 19230, 19241, 19518,
	19662, 19950, 20094, 20382, 20526
}};

static constexpr std::array<uint32_t, 40> g_uselessSegTripleKeys = {{
	362183, 362249, 371687, 371753, 588706, 588772, 598210, 598276,
	815229, 815295, 824733, 824799, 1041752, 1041818, 1051256, 1051322,
	1268275, 1268341, 1277779, 1277845, 1979591, 1979657, 1989095, 1989161,
	2206114, 2206180, 2215618, 2215684, 2432637, 2432703, 2442141, 2442207,
	2659160, 2659226, 2668664, 2668730, 2885683, 2885749, 2895187, 2895253
}};

static constexpr auto g_uselessSegPairBits =
	makeKeyBits<(USELESS_SEG_PAIR_KEY_SPACE + 63) / 64>(g_uselessSegPairKeys);
static constexpr auto g_uselessSegTripleBits =
	makeKeyBits<(USELESS_SEG_TRIPLE_KEY_SPACE + 63) / 64>(g_uselessSegTripleKeys);

static inline bool isUselessSegPair(int c, int d, int e, int f) {
	uint32_t key = (uint32_t)(((c*12+d)*12+e)*12+f);
	return keyBitSet(g_uselessSegPairBits, key);
}

static inline bool isUselessSegTriple(int c, int d, int e, int f, int g, int h) {
	uint32_t key = (uint32_t)(((((c*12+d)*12+e)*12+f)*12+g)*12+h);
	return keyBitSet(g_uselessSegTripleBits, key);
}

// solver for fully concrete positions
class PositionSolver {
	public:
	int e0,e1,e2,c0,c1,c2;
	// shp2 is the E-mirror of shp
	int shp,shp2,middle;
	// for PositionSolver shpx and shpx2 mirror shp/shp2
	int shpx, shpx2;
	FullPosition fp;
	ShapeTranTable& stt;
	ShpColTranTable& scte;
	ShpColTranTable& sctc;
	// PrunTables and CubePrunTables are mutually exclusive (the unused one is null).
	// Pointers (not references) so main() can skip building the unused pair.
	PrunTable* pr1;
	PrunTable* pr2;
	CubePrunTable* cpr1;
	CubePrunTable* cpr2;

	int moveList[50];
	int moveLen;
	int lastTurns[6];
	bool findAll;
	bool ignoreTrans;
	// doTop() amount applied as preAUF
	int m_preAUF{0};
	// doBot() amount applied as preADF
	int m_preADF{0};

	/*
	 * U2/D2 handling:
	 * - allowed in preabf and postabf
	 * - only allowed for a depth if no solutions without U2/D2 can be found (a "clean" solution)
	 * - before a clean solution, all dirty solutions are stored, and the buffer is cleared
	 *   upon the first clean

	 * m_slicesDone   – slices performed so far on the current search path
	 * m_internalBad  – how many U2/D2 internal moves exist in this path?
	 * m_cleanFound   – a clean solution exists at the current depth.
	 * m_dirtyBuf     – dirty solutions held back at the current depth
	*/
	int m_slicesDone{0};
	int m_internalBad{0};
	bool m_cleanFound{false};
	bool m_solutionFound{false};
	std::vector<std::string> m_dirtyBuf;
	bool m_cubeshape{false};
	clock_t m_lastProgressClock{0};

	// precomputed mapping from CS index to if a slice from there stays in CS
	std::vector<char> m_sliceStaysCubePrimary;

	// Emit the held-back solutions with U2/D2 for a depth that produced no clean solution.
	// Honors single-solution mode.
	// Returns whether at least one solution was emitted.
	bool emitDirtyBuffer() {
		bool emitted = false;
		for (const auto& s : m_dirtyBuf) {
			std::cout << s << std::flush;
			emitted = true;
			if (!findAll) break;
		}
		m_dirtyBuf.clear();
		return emitted;
	}

	PositionSolver( ShapeTranTable& stt0, ShpColTranTable& scte0, ShpColTranTable& sctc0, PrunTable* pr10, PrunTable* pr20, CubePrunTable* cpr10, CubePrunTable* cpr20 )
		: stt(stt0), scte(scte0), sctc(sctc0), pr1(pr10), pr2(pr20), cpr1(cpr10), cpr2(cpr20)
	{
		m_sliceStaysCubePrimary.resize(NUMSHAPES);
		for (int s = 0; s < NUMSHAPES; s++) {
			m_sliceStaysCubePrimary[s] = isCubeShape(stt.tranTable[s][2]) ? 1 : 0;
		}
	}
	// "is it good squares right now?"
	virtual bool checkKeepCubeShape() {
		return isCubeShape(shp); // shp2's CS is the mirror of shp
	}
	// "would it be in CS after a slice?"
	virtual inline bool sliceStaysCubeShape() {
		return m_sliceStaysCubePrimary[shp] != 0;
	}
	void set(FullPosition& p, bool findAll0, bool ignoreTrans0){
		int cc0 = p.getCornerColoring(0);
		int cc1 = p.getCornerColoring(1);
		int cc2 = p.getCornerColoring(2);
		c0 = (cc0==-1 ? -1 : sctc.ct.choice2Idx[cc0]);
		c1 = (cc1==-1 ? -1 : sctc.ct.choice2Idx[cc1]);
		c2 = (cc2==-1 ? -1 : sctc.ct.choice2Idx[cc2]);
		int ec0 = p.getEdgeColoring(0);
		int ec1 = p.getEdgeColoring(1);
		int ec2 = p.getEdgeColoring(2);
		e0 = (ec0==-1 ? -1 : scte.ct.choice2Idx[ec0]);
		e1 = (ec1==-1 ? -1 : scte.ct.choice2Idx[ec1]);
		e2 = (ec2==-1 ? -1 : scte.ct.choice2Idx[ec2]);
		shp = stt.getShape(p.getShape(),p.getParityOdd());
		shp2 = stt.tranTable[shp][3];
		shpx = shp;
		shpx2 = shp2;
		middle = p.middle;
		findAll=findAll0;
		ignoreTrans=ignoreTrans0;
		fp = p;
	};
	virtual inline int doMove(int m){
		const int mirrmv[3]={1,0,2};
		int r=0;
		if(m==0){
			r=stt.getTopTurn(shp);
		}else if(m==1){
			r=stt.getBotTurn(shp);
		}else{
			middle=-middle;
		}
		c0 = sctc.tranTable[shp][c0][m];
		c1 = sctc.tranTable[shp][c1][m];
		e0 = scte.tranTable[shp][e0][m];
		e1 = scte.tranTable[shp][e1][m];
		shp = stt.tranTable[shp][m];

		c2 = sctc.tranTable[shp2][c2][mirrmv[m]];
		e2 = scte.tranTable[shp2][e2][mirrmv[m]];
		shp2 = stt.tranTable[shp2][mirrmv[m]];
		return r;
	}
	virtual int solve(int twoGen, int extraMoves, bool keepCubeShape){
		m_cubeshape = keepCubeShape;
		m_solutionFound = false;
		// (preAUF,preADF) pairs, 2-gen-filtered and symmetry-deduped
		auto preABFs = symmetricPreABF(fp.pos, twoGen, specificAngleBot, specificAngleTop);
		if (preABFs.empty()) return 19;

		if (keepCubeShape) {
			if (!checkKeepCubeShape()) {
				return 19;
			}
			if ((twoGen == 1 || twoGen == 2) && !cornersAre2GenSolvable(fp.pos, twoGen, specificAngleBot)) {
				return 19;
			}
		}

		FullPosition fpOrig = fp;

		// do each of the preABF and then fix both layers
		struct PreABFState { FullPosition fp; int e0,e1,e2,c0,c1,c2,shp,shp2,middle,preAUF,preADF; };
		std::vector<PreABFState> states;
		for (const auto& kv : preABFs) {
			fp = fpOrig;
			if (kv.first  != 0) fp.doTop(kv.first);
			if (kv.second != 0) fp.doBot(kv.second);
			set(fp, findAll, ignoreTrans);
			states.push_back({fp, e0,e1,e2,c0,c1,c2,shp,shp2,middle,kv.first,kv.second});
		}
		const int sharedMiddle = states[0].middle;

		auto restore = [&](const PreABFState& st){
			fp=st.fp;
			e0=st.e0; e1=st.e1; e2=st.e2; c0=st.c0; c1=st.c1; c2=st.c2;
			shp=st.shp; shp2=st.shp2; middle=st.middle;
			m_preAUF=st.preAUF; m_preADF=st.preADF;
			moveLen=0; for(int i=0;i<6;i++) lastTurns[i]=0;
			m_slicesDone=0; m_internalBad=0;
		};

		unsigned long nodes=0;
		int optimalMoves = -1;
		m_dirtyBuf.clear();

		// candidates run in PARALLEL within the same depth
		if (!specificDepths.empty()) {
			for (int depth : specificDepths) {
				if (metric == SLICE_METRIC && ((depth % 2 == 1 && sharedMiddle == 1) || (depth % 2 == 0 && sharedMiddle == -1))) {
					std::cout << "depth "<<depth<<" does not match the barflip state" << std::endl<<std::flush;
					continue;
				}
				if(verbosity>=5) std::cout<<"searching depth "<<depth<<std::endl<<std::flush;
				m_cleanFound = false; m_dirtyBuf.clear();
				for (const auto& st : states) {
					if (stopRequested.load()) return -1;
					restore(st);
					int searchResult = search(depth, 1, &nodes, twoGen, keepCubeShape, specificAngleTop, specificAngleBot);
					if (searchResult < 0) return searchResult;
					if (searchResult != 0 && !findAll && (metric != SLICE_METRIC || m_cleanFound)) { fp = fpOrig; m_preAUF = 0; m_preADF = 0; return 0; }
				}
				if (metric == SLICE_METRIC && !m_cleanFound && emitDirtyBuffer() && !findAll) { fp = fpOrig; m_preAUF = 0; m_preADF = 0; return 0; }
			}
		} else {
			int l=-1;
			if (metric == SLICE_METRIC && sharedMiddle==1) l=-2;
			while(true){
				l++;
				if (metric == SLICE_METRIC && sharedMiddle!=0) l++;
				if(verbosity>=5) std::cout<<"searching depth "<<l<<std::endl<<std::flush;
				m_cleanFound = false; m_dirtyBuf.clear();
				bool anySol = false;
				for (const auto& st : states) {
					if (stopRequested.load()) return -1;
					restore(st);
					int searchResult = search(l, 1, &nodes, twoGen, keepCubeShape, specificAngleTop, specificAngleBot);
					if (searchResult < 0) return searchResult;
					if (searchResult != 0) {
						anySol = true;
						if (!findAll && (metric != SLICE_METRIC || m_cleanFound)) { fp = fpOrig; m_preAUF = 0; m_preADF = 0; return 0; }
					}
				}
				if (metric == SLICE_METRIC && !m_cleanFound) {
					if (emitDirtyBuffer() && !findAll) { fp = fpOrig; m_preAUF = 0; m_preADF = 0; return 0; }
				}
				if (anySol && optimalMoves == -1) optimalMoves = l;
				if (optimalMoves != -1 &&
				    (l >= optimalMoves + extraMoves || (metric == SLICE_METRIC && sharedMiddle!=0 && l+1 >= optimalMoves + extraMoves)))
					break;
			}
		}

		fp = fpOrig;
		m_preAUF = 0;
		m_preADF = 0;
		return 0;
	}
	virtual inline bool isSolved() {
		if( shp==4163 && e0==69 && e1==44 && e2==44 && c0==69 && c1==44 && c2==44 && middle>=0 ) return true;
		else return false;
	}
	// determine if we should prune this branch of the tree
	virtual inline bool prunedOut(int l) {
		if( m_cubeshape ){
			// loc1/2 >= 0 is just a sanity check. they should be in CS.
			int loc  = cubeRaw2Local(shp);
			int loc2 = cubeRaw2Local(shp2);
			if( loc>=0  && cpr1->table[loc ][e0][c0]>l+1 ) return true;
			if( loc>=0  && cpr2->table[loc ][e1][c1]>l+1 ) return true;
			if( loc2>=0 && cpr2->table[loc2][e2][c2]>l+1 ) return true;
			return false;
		}
		if( pr1->table[shp ][e0][c0]>l+1 ) return true;
		if( pr2->table[shp ][e1][c1]>l+1 ) return true;
		if( pr2->table[shp2][e2][c2]>l+1 ) return true;
		return false;
	}
	int search( const int l, const int lm, unsigned long *nodes, int twoGen, bool keepCubeShape, bool keepAngleTop, bool keepAngleBot){
		int i,r=0;
		if (stopRequested.load()) return -1;

		// search for l more moves. previous move was lm.
		(*nodes)++;
		if (*nodes % 5000 == 0) {
			clock_t now = clock();
			if (now - m_lastProgressClock >= CLOCKS_PER_SEC / 2) {
				m_lastProgressClock = now;
				std::cout << "__PROGRESS__ nodes=" << *nodes << " depth=" << l << std::endl << std::flush;
			}
		}
		if( l<0 ) return 0;

		// prune turn metric based on transformation
		// (a,b)/(c,d)/(e,f) -> (6+a,6+b)/(d,c)/(6+e,6+f)
		if( metric == TURN_METRIC && !ignoreTrans && twoGen == 0){
			// (a,b)/(c,d)/(e,f) -> (6+a,6+b)/(d,c)/(6+e,6+f)
			// moves changes by:
			// a,b,e,f=0/6 -> m++/m--
			i=0;
			if( lastTurns[0]==0 ) i++;
			else if( lastTurns[0]==6 ) i--;
			if( lastTurns[1]==0 ) i++;
			else if( lastTurns[1]==6 ) i--;
			if( lastTurns[4]==0 ) i++;
			else if( lastTurns[4]==6 ) i--;
			if( lastTurns[5]==0 ) i++;
			else if( lastTurns[5]==6 ) i--;
			int absTopMove = lastTurns[0]>6 ? 12-lastTurns[0] : lastTurns[0];
			int absBottomMove = lastTurns[1]>6 ? 12-lastTurns[1] : lastTurns[1];
			if( i<0 || ( i==0 && ((absTopMove + absBottomMove > 6) || (absTopMove + absBottomMove == 6 && absTopMove < absBottomMove)))) return 0;
		}

		// check if it is now solved
		if( l==0 ){
			if(isSolved()){
				printsol();
				if(verbosity>=6) std::cout<<"Nodes="<<*nodes<<std::endl<<std::flush;
				return 1;
			}else if( metric != SLICE_METRIC ) return 0;
		}

		// prune
		if(prunedOut(l)) return 0;

		// try all top layer moves
		if( lm>=2 ){
			i=doMove(0);
			do{
				// qq note: Jaap's solver pruned the transformation by only allowing U moves between
				// 0 and 5. I think it's better to do this on D, see below. We then allow any (x,0).
				int absTopMove = i>6 ? 12-i : i;
				if (absTopMove <= maxX && absTopMove <= maxTotal && (!keepAngleTop || absTopMove < 2)) {
					moveList[moveLen++]=i;
					lastTurns[4]=i;
					r+=search( metric==TURN_METRIC?l-1:metric==ANGLE_METRIC?l-absTopMove:l, 0, nodes, twoGen, keepCubeShape, keepAngleTop, keepAngleBot);
					moveLen--;
					if(r<0) return r;
					if(r!=0 && !findAll && (metric!=SLICE_METRIC || m_cleanFound)) return(r);
				}
				i+=doMove(0);
			}while( i<12);
			lastTurns[4]=0;
		}
		// try all bot layer moves
		// 2g/p2g D move restrictions are enforced at slices instead, so preADF/postADF work.
		if( lm!=1 ){
			i=doMove(1);
			do{
				// MOVE EQUIVALENCE PRUNE
				// can use transformation, no 2gen, and not in the last two moves,
				// then we should skip this move if the current (u,d) is worse than the y2.
				// meaning: |u| + |d| >= 7, or |u| + |d| = 6 and |d| > |u|
				int topMove = lastTurns[4];
				int absTopMove = topMove>6 ? 12-topMove : topMove;
				int absBottomMove = i>6 ? 12-i : i;
				// use the following to respect generator's solution inversion
				bool nearExemptBoundary = generator ? (m_slicesDone < 2) : (l < 2) || (m_slicesDone == 0);
				// explicitly keep both branches for T° and E*.
				// bottom conversion: raw = (12 - praw(printed)) % 12, praw(v) = v<0 ? v+12 : v.
				bool isTMove = (topMove==2 && i==4) || (topMove==10 && i==8)
					|| (topMove==3 && i==3) || (topMove==9 && i==9);
				if ((absBottomMove <= maxY) && (absBottomMove + absTopMove <= maxTotal) && (metric==TURN_METRIC || ignoreTrans || twoGen!=0 || nearExemptBoundary || isTMove || (absTopMove + absBottomMove < 6) || (absTopMove + absBottomMove == 6 && absTopMove >= absBottomMove))  && (!keepAngleBot || absBottomMove < 2)) {
					moveList[moveLen++]=i+12;
					lastTurns[5]=i;
					r+=search( metric==TURN_METRIC?l-1:metric==ANGLE_METRIC?l-absBottomMove:l, 1, nodes, twoGen, keepCubeShape, keepAngleTop, keepAngleBot);
					moveLen--;
					if(r<0) return r;
					if(r!=0 && !findAll && (metric!=SLICE_METRIC || m_cleanFound)) return(r);
				}
				i+=doMove(1);
			}while( i<12);
			lastTurns[5]=0;
		}
		// try slice move
		if( lm!=2 && l>0){
			// This slice closes (lastTurns[4],lastTurns[5]).
			// block if the U2/D2 is internal, and we've found a clean solution
			bool badSeg = (lastTurns[4]==6 && lastTurns[5]==0) || (lastTurns[4]==0 && lastTurns[5]==6);
			bool internalBadSeg = (metric == SLICE_METRIC) && badSeg && (m_slicesDone >= 1);
			bool block60 = internalBadSeg && m_cleanFound;
			// 2g/p2g D restrictions.
			// The start position is already the state after preADF, so the first D is 0.
			// isSolved() is exempt to not kill a solved state.
			bool blockTwoGen = false;
			if (twoGen != 0) {
				int d = lastTurns[5];
				int absD = (d > 6) ? 12 - d : d;
				bool disallowedD = (twoGen == 2) ? (d != 0) : (absD > 1);
				blockTwoGen = disallowedD && !isSolved();
			}
			// make sure the useless pair/triple has a leading slice (i.e. is not preABF)
			bool uselessSeg = false;
			if (!block60 && !blockTwoGen) {
				uselessSeg = (m_slicesDone >= 2) &&
					isUselessSegPair(lastTurns[2], lastTurns[3], lastTurns[4], lastTurns[5]);
				if (!uselessSeg) {
					uselessSeg = (m_slicesDone >= 3) &&
						isUselessSegTriple(lastTurns[0], lastTurns[1], lastTurns[2], lastTurns[3], lastTurns[4], lastTurns[5]);
				}
			}
			if (!block60 && !blockTwoGen && !uselessSeg && (!keepCubeShape || sliceStaysCubeShape())) {
				// -c check is done by sliceStaysCubeShape() already
				int lt0=lastTurns[0], lt1=lastTurns[1];
				lastTurns[0]=lastTurns[2];
				lastTurns[1]=lastTurns[3];
				lastTurns[2]=lastTurns[4];
				lastTurns[3]=lastTurns[5];
				lastTurns[4]=0;
				lastTurns[5]=0;
				doMove(2);
				m_slicesDone++;
				if (internalBadSeg) m_internalBad++;
				moveList[moveLen++]=0;
				// note that if angle metric is defined to count slices as 0, it will sometimes miss the optimal solution because the current pruning tables count how many slices a position is away from solved
				r+=search(l-1, 2, nodes, twoGen, keepCubeShape, false, false);
				moveLen--;
				if (internalBadSeg) m_internalBad--;
				m_slicesDone--;
				if(r<0) return r;
				if(r!=0 && !findAll && (metric!=SLICE_METRIC || m_cleanFound)) return(r);
				doMove(2);
				lastTurns[5]=lastTurns[3];
				lastTurns[4]=lastTurns[2];
				lastTurns[3]=lastTurns[1];
				lastTurns[2]=lastTurns[0];
				lastTurns[1]=lt1;
				lastTurns[0]=lt0;
			} // end slice guard
		}
		return r;
	}

	int normalizeMove(int m){
		while(m<0) m+=12;
		while(m>=12) m-=12;
		if( usenegative && m>6 ) m-=12;
		return m;
	}
	std::string printmove(int mu, int md){
		std::string out = "";
		if( mu!=0 || md!=0 ) {
			if( usebrackets && !karnotation ) out += "(";
			out += std::to_string(mu);
			out += ",";
			out += std::to_string(md);
			if( usebrackets && !karnotation ) out += ")";
		}
		return out;
	}
	/*
	 * Abid's notation is WCA with slash → space, except for leading or trailing slice or a slice start.
	 *
	 * hasIndicator = has a slice start
	 */
	static std::string abidSpacing(const std::string& alg, bool hasIndicator = false){
		size_t markerAt = hasIndicator ? alg.find_first_of("/\\|") : std::string::npos;
		size_t firstIdx = alg.find_first_not_of(" \t");
		size_t lastIdx = alg.find_last_not_of(" \t");
		if( firstIdx == std::string::npos || lastIdx == std::string::npos ) return alg;
		std::string out;
		out.reserve(alg.size());
		for( size_t i=0; i<alg.size(); i++ ){
			char ch = alg[i];
			if( ch != '/' ){ out += ch; continue; }
			bool leading = (i==firstIdx);
			bool trailing = (i==lastIdx);
			bool isMarker = (i==markerAt);
			if( leading || trailing || isMarker ) out += ch;
			else out += ' ';
		}
		return out;
	}
	// inject slice start
	static std::string injectSliceIndicator(const std::string& alg, const std::string& indicator){
		if( indicator.empty() ) return alg;
		size_t sep = alg.find_first_of("/\\| ");
		if( sep == std::string::npos ) return alg;
		return alg.substr(0, sep) + indicator + alg.substr(sep+1);
	}
	void printsol(){
		m_solutionFound = true;
		std::string out = "";
		int tw=0, tu=0;
		int mu=0, md=0;
		int angle=0;

		if( generator ){
			for( int i=moveLen-1; i>=0; i--){
				if( moveList[i]==0 ) {
					out += printmove(mu, md);
					mu = md = 0;
					out += "/";
					tu++; tw++; angle++;
				}else if( moveList[i]<12 ){
					mu = normalizeMove(mu-moveList[i]);
					tu++;
					angle += (mu<0?-mu:mu);
				}else{
					md = normalizeMove(md+moveList[i]);
					tu++;
					angle += (md<0?-md:md);
				}
			}
			// Generator: preABF is at the end, negated.
			if (m_preAUF != 0)
				mu = normalizeMove(mu - m_preAUF);
			if (m_preADF != 0)
				md = normalizeMove(md - m_preADF);
		}else{
			// Solver: preABF happens first.
			mu = normalizeMove(m_preAUF);
			md = normalizeMove(m_preADF);
			for( int i=0; i<moveLen; i++){
				if( moveList[i]==0 ) {
					out += printmove(mu, md);
					mu = md = 0;
					out += "/";
					tu++; tw++; angle++;
				}else if( moveList[i]<12 ){
					mu = normalizeMove(mu+moveList[i]);
					tu++;
					angle += (mu<0?-mu:mu);
				}else{
					md = normalizeMove(md-moveList[i]);
					tu++;
					angle += (md<0?-md:md);
				}
			}
		}
		out += printmove(mu, md);
		// Save raw algorithm before karnotation transform (for the bridge)
		std::string rawAlg = out;
		bool useSmartKarn = (karnotation == 2) && !m_cubeshape;
		std::string karnConverted;
		if (karnotation || g_extendedOutput)
			karnConverted = useSmartKarn ? karnifycs(rawAlg, fp.pos, generator) : karnify(rawAlg);
		if (karnotation && !g_extendedOutput)
			out = (karnotation == 3) ? abidSpacing(rawAlg) : karnConverted;
		std::string line = out + "  [" + std::to_string(tw);
		if (metric != SLICE_METRIC) line += "|" + std::to_string(tu);
		if (metric == ANGLE_METRIC) line += "|" + std::to_string(angle);
		line += "]";
		if (g_extendedOutput) {
			line += "  " + karnConverted;
			// ergo rating
			std::string sliceMarker;
			if (m_cubeshape) {
				bool initialTopA = (fp.pos[0] >= 8);
				try {
					const RatingWeights w = getRatingWeights();
					AlgRating rating = rateAlg(rawAlg, initialTopA, w.w1, w.w2, w.w3, w.w4);
					if (rating.valid) {
						sliceMarker = rating.sliceStart;
						std::string safeSS = rating.sliceStart;
						if (safeSS == "\\") safeSS = "\\\\";
						else if (safeSS == "\"") safeSS = "\\\"";
						line += "  R{\"f\":" + std::to_string(rating.FINAL)
						     + ",\"ss\":\"" + safeSS + "\""
						     + ",\"p1\":" + std::to_string(rating.PHASE1)
						     + ",\"p2\":" + std::to_string(rating.PHASE2)
						     + ",\"p3\":" + std::to_string(rating.PHASE3)
						     + ",\"p4\":" + std::to_string(rating.PHASE4)
						     + ",\"w1\":" + std::to_string(rating.W1)
						     + ",\"w2\":" + std::to_string(rating.W2)
						     + ",\"w3\":" + std::to_string(rating.W3)
						     + ",\"w4\":" + std::to_string(rating.W4)
						     + ",\"eu\":" + std::to_string(rating.ergo_up)
						     + ",\"ed\":" + std::to_string(rating.ergo_down)
						     + ",\"sc\":" + std::to_string(rating.sliceCount)
						     + ",\"mv\":" + std::to_string(rating.movement)
						     + ",\"bn\":" + std::to_string(rating.bonus) + "}";
					}
				} catch (...) { }
			}
			// Abid notation (karnotation == 3) is produced solver-side, for speed.
			if (karnotation == 3) {
				std::string abidAlg = sliceMarker.empty()
					? rawAlg
					: injectSliceIndicator(rawAlg, sliceMarker);
				line += "  " + abidSpacing(abidAlg, !sliceMarker.empty());
			}
		}
		line += " \n";
		if (metric != SLICE_METRIC) { std::cout << line << std::flush; return; }
		if (m_internalBad > 0) {
			if (findAll || m_dirtyBuf.empty()) m_dirtyBuf.push_back(line);
		} else {
			if (!m_cleanFound) {
				m_cleanFound = true;
				m_dirtyBuf.clear();
			}
			std::cout << line << std::flush;
		}
	}
};

// PartialPositionSolver is PositionSolver with partial pieces (partly defined)
class PartialPositionSolver : public PositionSolver {
public:
	PartialPositionSolver( ShapeTranTable& stt0, ShpColTranTable& scte0, ShpColTranTable& sctc0, PrunTable* pr10, PrunTable* pr20, CubePrunTable* cpr10, CubePrunTable* cpr20 )
	    : PositionSolver(stt0, scte0, sctc0, pr10, pr20, cpr10, cpr20) {}

	// Optional shared cache for reusing dynamic pruning tables (set by the batch solver)
	DynTableCache* dynCache = nullptr;

	// Dynamic per-query pruning tables for -c (DynCubePrunTable1D).
	// When more than 4 of one piece type are known, one table per 4-subset (capped),
	// and tracked through doMove() like c0/e0.
	static const size_t MAX_DYN_COMBOS = 16;
	std::vector<DynCubePrunTable1D> dynCornerTables;
	std::vector<DynCubePrunTable1D> dynEdgeTables;
	std::vector<std::array<int,4>> dynCornerMarked;
	std::vector<std::array<int,4>> dynEdgeMarked;
	std::vector<int> dynCornerIdx;
	std::vector<int> dynEdgeIdx;

	void buildDynamicTables(const int pos[24]){
		dynCornerTables.clear(); dynCornerMarked.clear();
		dynEdgeTables.clear();   dynEdgeMarked.clear();

		std::vector<int> knownCorners, knownEdges;
		for(int i=0;i<24;i++){
			int v=pos[i];
			if(v>=0 && v<8){ knownCorners.push_back(v); i++; } // corners occupy 2 slots
			else if(v>=8 && v<16) knownEdges.push_back(v);
		}
		if((int)knownCorners.size()>=4){
			auto combos = chooseFour(knownCorners);
			if(combos.size()>MAX_DYN_COMBOS) combos.resize(MAX_DYN_COMBOS);
			for(auto& combo : combos){
				dynCornerMarked.push_back(combo);
				if (dynCache)
					dynCornerTables.emplace_back(*dynCache->get(combo, false, stt, sctc, sctc.ct));
				else
					dynCornerTables.emplace_back(combo.data(), false, stt, sctc, sctc.ct);
			}
		}
		if((int)knownEdges.size()>=4){
			auto combos = chooseFour(knownEdges);
			if(combos.size()>MAX_DYN_COMBOS) combos.resize(MAX_DYN_COMBOS);
			for(auto& combo : combos){
				dynEdgeMarked.push_back(combo);
				if (dynCache)
					dynEdgeTables.emplace_back(*dynCache->get(combo, true, stt, scte, scte.ct));
				else
					dynEdgeTables.emplace_back(combo.data(), true, stt, scte, scte.ct);
			}
		}
		dynCornerIdx.assign(dynCornerTables.size(), -1);
		dynEdgeIdx.assign(dynEdgeTables.size(), -1);
	}
	// Recompute each dynamic table's color index from a (possibly preADF-rotated) position. Cheap.
	void refreshDynIdx(const int pos[24]){
		for(size_t i=0;i<dynCornerTables.size();i++)
			dynCornerIdx[i] = markedColorIdx(pos, dynCornerMarked[i].data(), false, sctc.ct);
		for(size_t i=0;i<dynEdgeTables.size();i++)
			dynEdgeIdx[i] = markedColorIdx(pos, dynEdgeMarked[i].data(), true, scte.ct);
	}

	// either parity, for both functions below.
	bool checkKeepCubeShape() override {
		return isCubeShape(shp) || isCubeShape(shpx);
	}
	inline bool sliceStaysCubeShape() override {
		return (m_sliceStaysCubePrimary[shp] || m_sliceStaysCubePrimary[shpx]) != 0;
	}
	void set(FullPosition& p, bool findAll0, bool ignoreTrans0){
		PositionSolver::set(p, findAll0, ignoreTrans0);
		shpx = stt.getShape(p.getShape(),!p.getParityOdd());
		shpx2 = stt.tranTable[shpx][3];
		refreshDynIdx(p.pos);
	};
	inline int doMove(int m) override {
		const int mirrmv[3]={1,0,2};
		int r=0;
		if(m==0){
			r=stt.getTopTurn(shp);
			fp.doTop(r);
		}else if(m==1){
			r=stt.getBotTurn(shp);
			fp.doBot(-r);
		}else{
			middle=-middle;
			fp.doSlice();
		}
		// only update c0/c1/e0/e1/c2/e2 if they are not -1
		if (c0>-1) c0 = sctc.tranTable[shp][c0][m];
		if (c1>-1) c1 = sctc.tranTable[shp][c1][m];
		if (e0>-1) e0 = scte.tranTable[shp][e0][m];
		if (e1>-1) e1 = scte.tranTable[shp][e1][m];
		// maintain color indices for the dynamic single-axis pruning tables
		for (size_t i=0; i<dynCornerIdx.size(); i++)
			if (dynCornerIdx[i]>-1) dynCornerIdx[i] = sctc.tranTable[shp][dynCornerIdx[i]][m];
		for (size_t i=0; i<dynEdgeIdx.size(); i++)
			if (dynEdgeIdx[i]>-1) dynEdgeIdx[i] = scte.tranTable[shp][dynEdgeIdx[i]][m];
		shp = stt.tranTable[shp][m];
		shpx = stt.tranTable[shpx][m];

		if (c2>-1) c2 = sctc.tranTable[shp2][c2][mirrmv[m]];
		if (e2>-1) e2 = scte.tranTable[shp2][e2][mirrmv[m]];
		shp2 = stt.tranTable[shp2][mirrmv[m]];
		shpx2 = stt.tranTable[shpx2][mirrmv[m]];
		return r;
	}
	int solve(int twoGen, int extraMoves, bool keepCubeShape) override {
		m_cubeshape = keepCubeShape;
		m_solutionFound = false;
		// (preAUF,preADF) pairs, 2-gen-filtered and symmetry-deduped
		auto preABFs = symmetricPreABF(fp.pos, twoGen, specificAngleBot, specificAngleTop);
		if (preABFs.empty()) return 19;

		if (keepCubeShape) {
			if (!checkKeepCubeShape()) {
				return 19;
			}
			if ((twoGen == 1 || twoGen == 2) && !cornersAre2GenSolvable(fp.pos, twoGen, specificAngleBot)) {
				return 19;
			}
		}

		FullPosition fpOrig = fp;

		// ok to pass in pre-preABF position to pruning tables
		if (keepCubeShape) buildDynamicTables(fpOrig.pos);

		// do each of the preABF and then fix both layers
		struct PreABFState {
			FullPosition fp;
			int e0,e1,e2,c0,c1,c2,shp,shp2,shpx,shpx2,middle,preAUF,preADF;
			std::vector<int> dynCornerIdx;
			std::vector<int> dynEdgeIdx;
		};
		std::vector<PreABFState> states;
		for (const auto& kv : preABFs) {
			fp = fpOrig;
			if (kv.first  != 0) fp.doTop(kv.first);
			if (kv.second != 0) fp.doBot(kv.second);
			set(fp, findAll, ignoreTrans);
			states.push_back({fp, e0,e1,e2,c0,c1,c2,shp,shp2,shpx,shpx2,middle,kv.first,kv.second, dynCornerIdx, dynEdgeIdx});
		}
		const int sharedMiddle = states[0].middle;

		auto restore = [&](const PreABFState& st){
			fp=st.fp;
			e0=st.e0; e1=st.e1; e2=st.e2; c0=st.c0; c1=st.c1; c2=st.c2;
			shp=st.shp; shp2=st.shp2; shpx=st.shpx; shpx2=st.shpx2;
			middle=st.middle; m_preAUF=st.preAUF; m_preADF=st.preADF;
			dynCornerIdx=st.dynCornerIdx; dynEdgeIdx=st.dynEdgeIdx;
			moveLen=0; for(int i=0;i<6;i++) lastTurns[i]=0;
			m_slicesDone=0; m_internalBad=0;
		};

		unsigned long nodes=0;
		int optimalMoves = -1;
		m_dirtyBuf.clear();

		if (!specificDepths.empty()) {
			for (int depth : specificDepths) {
				if (metric == SLICE_METRIC && ((depth % 2 == 1 && sharedMiddle == 1) || (depth % 2 == 0 && sharedMiddle == -1))) {
					std::cout << "depth "<<depth<<" does not match the barflip state" << std::endl<<std::flush;
					continue;
				}
				if(verbosity>=5) std::cout<<"searching depth "<<depth<<std::endl<<std::flush;
				m_cleanFound = false; m_dirtyBuf.clear();
				for (const auto& st : states) {
					if (stopRequested.load()) return -1;
					restore(st);
					int searchResult = search(depth, 1, &nodes, twoGen, keepCubeShape, specificAngleTop, specificAngleBot);
					if (searchResult < 0) return searchResult;
					if (searchResult != 0 && !findAll && (metric != SLICE_METRIC || m_cleanFound)) { fp = fpOrig; m_preAUF = 0; m_preADF = 0; return 0; }
				}
				if (metric == SLICE_METRIC && !m_cleanFound && emitDirtyBuffer() && !findAll) { fp = fpOrig; m_preAUF = 0; m_preADF = 0; return 0; }
			}
		} else {
			int l=-1;
			if( metric==SLICE_METRIC && sharedMiddle==1 ) l=-2;
			while(true){
				l++;
				if( metric==SLICE_METRIC && sharedMiddle!=0 ) l++;
				if(verbosity>=5) std::cout<<"searching depth "<<l<<std::endl<<std::flush;
				m_cleanFound = false; m_dirtyBuf.clear();
				bool anySol = false;
				for (const auto& st : states) {
					if (stopRequested.load()) return -1;
					restore(st);
					int searchResult = search(l, 1, &nodes, twoGen, keepCubeShape, specificAngleTop, specificAngleBot);
					if (searchResult < 0) return searchResult;
					if (searchResult != 0) {
						anySol = true;
						if (!findAll && (metric != SLICE_METRIC || m_cleanFound)) { fp = fpOrig; m_preAUF = 0; m_preADF = 0; return 0; }
					}
				}
				if (metric == SLICE_METRIC && !m_cleanFound) {
					if (emitDirtyBuffer() && !findAll) { fp = fpOrig; m_preAUF = 0; m_preADF = 0; return 0; }
				}
				if (anySol && optimalMoves == -1) optimalMoves = l;
				if (optimalMoves != -1 &&
				    (l >= optimalMoves + extraMoves || (metric==SLICE_METRIC && sharedMiddle!=0 && l+1 >= optimalMoves + extraMoves)))
					break;
			}
		}

		fp = fpOrig;
		m_preAUF = 0;
		m_preADF = 0;
		return 0;
	}
	inline bool isSolved() override {
		if (middle < 0) return false;
		if (!fp.matchesSolved()) return false;
		// at least one of the duplicated pieces must be solved
		for (size_t g=0; g<fp.duplicates.size(); g++) {
			int val = fp.pos[fp.duplicates[g].solvedPosIdx];
			bool found = false;
			for (int v=0; v<fp.duplicates[g].count; v++) {
				if (val == fp.duplicates[g].values[v]) {
					found = true;
					break;
				}
			}
			if (!found) return false;
		}
		return true;
	}
	// determine if we should prune this branch of the tree
	// we should have a shape-only pruning table
	inline bool prunedOut(int l) override {
		if( m_cubeshape ){
			/*
			 * loc and locx:
			 *  - both >=0: both in CS, so prune only if both failed pruning tables.
			 *  - either >=0: the <0 track is bad. so prune if the >=0 track fail the tables.
			 *  - both < 0: boooo (rejected by checkKeepCubeShape)
			 */
			int loc   = cubeRaw2Local(shp);
			int locx  = cubeRaw2Local(shpx);
			int loc2  = cubeRaw2Local(shp2);
			int locx2 = cubeRaw2Local(shpx2);
			if (e0>-1 && c0>-1) {
				bool a = (loc>=0)  && cpr1->table[loc ][e0][c0]>l+1;
				bool b = (locx>=0) && cpr1->table[locx][e0][c0]>l+1;
				if( a && b ) return true;
				// the case for either >=0:
				if( a && locx<0 ) return true;
				if( b && loc<0  ) return true;
			}
			if (e1>-1 && c1>-1) {
				bool a = (loc>=0)  && cpr2->table[loc ][e1][c1]>l+1;
				bool b = (locx>=0) && cpr2->table[locx][e1][c1]>l+1;
				if( a && b ) return true;
				if( a && locx<0 ) return true;
				if( b && loc<0  ) return true;
			}
			if (e2>-1 && c2>-1) {
				bool a = (loc2>=0)  && cpr2->table[loc2 ][e2][c2]>l+1;
				bool b = (locx2>=0) && cpr2->table[locx2][e2][c2]>l+1;
				if( a && b ) return true;
				if( a && locx2<0 ) return true;
				if( b && loc2<0  ) return true;
			}
			// the dynamic tables
			for (size_t i=0; i<dynCornerTables.size(); i++) {
				if (dynCornerIdx[i]<0) continue;
				bool a = (loc>=0)  && dynCornerTables[i].table[loc ][dynCornerIdx[i]]>l+1;
				bool b = (locx>=0) && dynCornerTables[i].table[locx][dynCornerIdx[i]]>l+1;
				if( a && b ) return true;
				if( a && locx<0 ) return true;
				if( b && loc<0  ) return true;
			}
			for (size_t i=0; i<dynEdgeTables.size(); i++) {
				if (dynEdgeIdx[i]<0) continue;
				bool a = (loc>=0)  && dynEdgeTables[i].table[loc ][dynEdgeIdx[i]]>l+1;
				bool b = (locx>=0) && dynEdgeTables[i].table[locx][dynEdgeIdx[i]]>l+1;
				if( a && b ) return true;
				if( a && locx<0 ) return true;
				if( b && loc<0  ) return true;
			}
			return false;
		}
		if (e0>-1 && c0>-1) {
			if( pr1->table[shp ][e0][c0]>l+1 && pr1->table[shpx][e0][c0]>l+1) return true;
		}
		if (e1>-1 && c1>-1) {
			if( pr2->table[shp ][e1][c1]>l+1 && pr2->table[shpx][e1][c1]>l+1) return true;
		}
		if (e2>-1 && c2>-1) {
			if( pr2->table[shp2][e2][c2]>l+1 && pr2->table[shpx2][e2][c2]>l+1) return true;
		}
		return false;
	}
};

int show(int e){
	std::cerr<<errors[e-1]<<std::endl;
	return(e);
}

// true if both layers start with the same piece type
static inline bool isSingleMisalign(const int pos[24]) {
	return pos[0] >= 8 == pos[12] >= 8;
}

// standalone CS check: edges are at positions i s.t. i%3==r for some r. apply to both layers.
static bool isInCubeshapeRaw(const int pos[24]) {
	for (int base = 0; base < 24; base += 12) {
		bool layerOk = false;
		for (int r = 0; r < 3; r++) {
			bool match = true;
			for (int i = 0; i < 12; i++) {
				if ((i % 3 == r) != (pos[base + i] >= 8)) { match = false; break; }
			}
			if (match) { layerOk = true; break; }
		}
		if (!layerOk) return false;
	}
	return true;
}

// Pre-validate position against keepCubeShape + twoGen constraints. Pruning tables not needed.
// Returns 0 if OK, error code 19 if not.
static int preValidate(FullPosition& p, bool keepCubeShape, int twoGen) {
	if (!keepCubeShape) return 0;
	if (!isInCubeshapeRaw(p.pos)) return 19;
	// getParityOdd() doesn't return even for "no parity"
	if (!p.isPartial() && p.getParityOdd() != isSingleMisalign(p.pos)) return 19;
	if ((twoGen == 1 || twoGen == 2) && !cornersAre2GenSolvable(p.pos, twoGen, specificAngleBot)) return 19;
	return 0;
}

int parseInteger(const char* s){
	int n=0;
	while( *s!='\0' ){
		if( *s<'0' || *s>'9' ) return -1;
		n = n*10 + (*s -'0');
		s++;
	}
	return n;
}

void help(){
	std::cout<<"Croissant Usage:"<<std::endl;
	std::cout<<"  croissant <flags> <state>"<<std::endl;
	std::cout<<"  croissant <flags> <moves>"<<std::endl;
	std::cout<<"  croissant <flags>"<<std::endl;
	std::cout<<std::endl;
	std::cout<<"<state> is a string encoding a particular cube state. For example"<<std::endl;
	std::cout<<"   A1B2C3D45E6F7G8H- is the solved position. Letters represent corners, numbers"<<std::endl;
	std::cout<<"   the edges, starting from the UFL corner (A) clockwise around the top layer and"<<std::endl;
	std::cout<<"   then clockwise around the bottom layer. Optionally, the middle layer is"<<std::endl;
	std::cout<<"   denoted by a - if it's not flipped and / if it is."<<std::endl;
	std::cout<<"   You can also partially define pieces:"<<std::endl;
	std::cout<<"   U is a top corner, V is a bottom corner, W is any corner,"<<std::endl;
	std::cout<<"   X is a top edge,   Y is a bottom edge,   Z is any edge."<<std::endl;
	std::cout<<"<moves> is a string encoding a sequence of moves. WCA style (no karn!)."<<std::endl;
	std::cout<<"   Parentheses are optional, but there must be no spaces inside the string."<<std::endl;
	std::cout<<"   With the -g flag active, the target state is obtained by doing these moves"<<std::endl;
	std::cout<<"   from the solved state. Without the flag active, doing the moves on the"<<std::endl;
	std::cout<<"   target state should be able to solve the cube."<<std::endl;
	std::cout<<"<flags> are one of more of the following command line flags:"<<std::endl;
	std::cout<<"   -es    Use slice metric (only slices count as moves; this is the default)."<<std::endl;
	std::cout<<"   -em    Use move/turn metric (layer turns count as well as slices)."<<std::endl;
	std::cout<<"   -ea    Use angle metric (a 3,0 layer count as 3 moves)."<<std::endl;
	std::cout<<"   -a<n>  Generate all optimal solutions, not just the first one found."<<std::endl;
	std::cout<<"          If n is given, also find solutions with up to n extra moves."<<std::endl;
	std::cout<<"   -d<n,> Search at a specific depth."<<std::endl;
	std::cout<<"          Requires a comma-separated list of depths after -d."<<std::endl;
	std::cout<<"          e.g. \"-d3,5\" under slice metric will search 3 and 5 slicers."<<std::endl;
	std::cout<<"   -x     Ignore the equivalence a,b/c,d/e,f = 6+a,6+b/d,c/6+e,6+f."<<std::endl;
	std::cout<<"          This will ACTUALLY generate all solutions: all the possible y2 algs."<<std::endl;
	std::cout<<"   -m     Ignore the bar state."<<std::endl;
	std::cout<<"   -b     Use parentheses when outputing layer turns."<<std::endl;
	std::cout<<"   -r<n>  Solve n random states, or infinitely many if n is 0 or missing."<<std::endl;
	std::cout<<"   -v<n>  Set verbosity, between 0 (minimal output) to 7 (full output)"<<std::endl;
	std::cout<<"   -h, --help  Show this help."<<std::endl;
	std::cout<<"   -g     Generator mode. Input/Output go from the solved state from the target state."<<std::endl;
	std::cout<<"          i.e. Non-generator mode outputs solutions, generator mode outputs setups."<<std::endl;
	std::cout<<"   -i<fn> Use as input each line from the file with filename <fn>."<<std::endl;
	std::cout<<"   -2     2gen - no bottom layer moves."<<std::endl;
	std::cout<<"   -p     Pseudo 2gen - only allow bottom layer moves of 1, 0, -1."<<std::endl;
	std::cout<<"   -c     Stay in cubeshape."<<std::endl;
	std::cout<<"   -k0    Output algs numerically (default)."<<std::endl;
	std::cout<<"   -k1    Output algs in karn."<<std::endl;
	std::cout<<"   -k2    Output algs in smart (cubeshape-aware) karn."<<std::endl;
	std::cout<<"   -k3    Output algs in Abid's notation (WCA with barred digits, slashes as spaces)."<<std::endl;
	std::cout<<"   -ob    Normalize ABF on both preABF and postABF."<<std::endl;
	std::cout<<"   -oe    Normalize ABF on preABF only (the move before the first slice)."<<std::endl;
	std::cout<<"   -os    Normalize ABF on postABF only (the move after the last slice)."<<std::endl;
	std::cout<<"   -nb    Lock both layer angles on pre-ABF (top and bottom)."<<std::endl;
	std::cout<<"          Useful for generating algs from a specific angle."<<std::endl;
	std::cout<<"   -nu    Lock top layer angle on pre-ABF only."<<std::endl;
	std::cout<<"   -nd    Lock bottom layer angle on pre-ABF only."<<std::endl;
	std::cout<<"          The above two flags are useful for avoiding 4x solutions for PBLs like H and Q."<<std::endl;
	std::cout<<"   -nn    No layer angle lock (default)."<<std::endl;
	std::cout<<"   -X<n>  Only allow top layer turns of a maximum of n in either direction."<<std::endl;
	std::cout<<"   -Y<n>  Only allow bottom layer turns of a maximum of n in either direction."<<std::endl;
	std::cout<<"   -Z<n>  Only allow turns of a maximum of n total turn amount (abs(X) + abs(Y))."<<std::endl;
}


int sq1optMain(int argc, char* argv[]){
	resetSolverOptions();
	bool ignoreMid=false;
	bool ignoreTrans=false;
	bool findAll=false;
	int twoGen = 0; // 0 = false, 1 = pseudo 2gen, 2 = 2gen
	int numpos = -1;
	char *inpFile=NULL;
	int posArg=-1;
	usenegative=true; // why would you not want negative turns?
	int extraMoves = 0;
	bool keepCubeShape = false;
	int parsedValue = 0;
	for( int i=1; i<argc; i++){
		if( std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "-help") == 0 || std::strcmp(argv[i], "/?") == 0 ){
			help();
			return 0;
		}
		if( argv[i][0]=='-' ){
			switch( argv[i][1] ){
				case 'e':
				case 'E':
					if (argv[i][2]=='s'||argv[i][2]=='S') metric=SLICE_METRIC;
					else if (argv[i][2]=='m'||argv[i][2]=='M') metric=TURN_METRIC;
					else if (argv[i][2]=='a'||argv[i][2]=='A') metric=ANGLE_METRIC;
					else return show(1);
					break;
				case 'x':
					ignoreTrans=true; break;
				case 'a':
				case 'A':
					findAll=true;
					extraMoves = parseInteger( argv[i]+2 );
					if( extraMoves<0 ) return show(15);
					break;
				case 'm':
				case 'M':
					ignoreMid=true; break;
				case 'b':
				case 'B':
					usebrackets=true; break;
				case 'r':
				case 'R':
					numpos = parseInteger( argv[i]+2 );
					if( numpos<0 ) return show(15);
					break;
				case 'v':
				case 'V':
					verbosity = parseInteger( argv[i]+2 );
					if( verbosity<0 ) return show(15);
					break;
				case 'h':
				case 'H':
					help();
					return 0;
				case 'g':
				case 'G':
					generator = true;
					break;
				case 'i':
				case 'I':
					inpFile=argv[i]+2; break;
				case 'p':
				case 'P':
					twoGen = 1;
					break;
				case '2':
					twoGen = 2;
					break;
				case 'c':
				case 'C':
					keepCubeShape = true;
					break;
				case 'k':
				case 'K':
					if (argv[i][2] == '0') karnotation = 0;
					else if (argv[i][2] == '2') karnotation = 2;
					else if (argv[i][2] == '3') karnotation = 3;
					else karnotation = 1;
					break;
				case 'n':
				case 'N':
					if (argv[i][2]=='b'||argv[i][2]=='B') { specificAngleTop=true; specificAngleBot=true; }
					else if (argv[i][2]=='u'||argv[i][2]=='U') specificAngleTop=true;
					else if (argv[i][2]=='d'||argv[i][2]=='D') specificAngleBot=true;
					else if (argv[i][2]=='n'||argv[i][2]=='N') { specificAngleTop=false; specificAngleBot=false; }
					else return show(1);
					break;
				case 'X':
					parsedValue = parseInteger(argv[i]+2);
					if (parsedValue >= 0 && parsedValue <= 6) maxX = parsedValue;
					break;
				case 'Y':
					parsedValue = parseInteger(argv[i]+2);
					if (parsedValue >= 0 && parsedValue <= 6) maxY = parsedValue;
					break;
				case 'Z':
					parsedValue = parseInteger(argv[i]+2);
					if (parsedValue >= 1 && parsedValue <= 12) maxTotal = parsedValue;
					break;
				case 'd':
				case 'D':
					{
						std::string ds(argv[i]+2);
						std::stringstream ss(ds);
						std::string token;
						while(std::getline(ss, token, ',')) {
							token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
							if (!token.empty()) {
								try { specificDepths.push_back(std::stoi(token)); } catch(...) {}
							}
						}
					}
					break;
				default:
					return show(1);
			}
		}else if( posArg<0 ){
			posArg = i;
		}else{
			return show(2);
		}
	}

	// don't use the equivalence if we want to limit move amounts
	if (maxX != 6 || maxY != 6 || maxTotal != 12) ignoreTrans = true;

	FullPosition p;
	std::ifstream is;
	bool havePosition = false;
	// use directly injected position if available
	if( s_hasInjectedPosition ){
		p.set(s_injectedPos, s_injectedMiddle);
		s_hasInjectedPosition = false;
		havePosition = true;
	}else if( posArg>=0 ){
		int r=p.parseInput(argv[posArg]);
		if(r) return show(r);
		havePosition = true;
	}else if( inpFile!=NULL ){
		is.open(inpFile);
		if(is.fail()) return show(3);
	}else if( numpos<0 ){
		help();
		return 0;
	}

	// now we have a position p to solve, so reject impossible params before initing the tables
	if (havePosition) {
		int pre = preValidate(p, keepCubeShape, twoGen);
		if (pre) return show(pre);
	}

	if(verbosity>=3) std::cout << "Initializing..."<<std::endl;
	// transition tables
	ChoiceTable ct;
	if(verbosity>=4) std::cout << "  5. Computing move table for CS"<<std::endl;
	ShapeTranTable st;
	if(verbosity>=4) std::cout << "  4. Computing move table for edges"<<std::endl;
	ShpColTranTable scte( st, ct, true );
	if(verbosity>=4) std::cout << "  3. Computing move table for corners"<<std::endl;
	ShpColTranTable sctc( st, ct, false );

	// pruning tables for two colorings
	FullPosition q;
	PrunTable* pr1 = nullptr;
	PrunTable* pr2 = nullptr;
	CubePrunTable* cpr1 = nullptr;
	CubePrunTable* cpr2 = nullptr;
	if (keepCubeShape) {
		// only the CubePrunTables are needed.
		if(verbosity>=4) std::cout << "  2. Computing restricted cubeshape pruning table #1"<<std::endl;
		cpr1 = new CubePrunTable(q, 0, st, scte, sctc);
		if(verbosity>=4) std::cout << "  1. Computing restricted cubeshape pruning table #2"<<std::endl;
		cpr2 = new CubePrunTable(q, 1, st, scte, sctc);
	} else {
		if(verbosity>=4) std::cout << "  2. Computing pruning table #1"<<std::endl;
		pr1 = new PrunTable(q, 0, st,scte,sctc );
		if(verbosity>=4) std::cout << "  1. Computing pruning table #2"<<std::endl;
		pr2 = new PrunTable(q, 1, st,scte,sctc );
	}
	if(verbosity>=4) std::cout << "  0. Finished."<<std::endl;
	PositionSolver ps( st, scte, sctc, pr1, pr2, cpr1, cpr2 );
	PartialPositionSolver pps( st, scte, sctc, pr1, pr2, cpr1, cpr2 );

	if(verbosity>=2){
		std::cout<<"Flags: "<<(metric==TURN_METRIC?"Move":metric==SLICE_METRIC?"Slice":"Angle")<<" Metric, ";
		std::cout<<"Find "<< (findAll? "every ":"first ");
		std::cout<< (generator? "generator":"solution");
		if (twoGen == 1) {
			std::cout << ", Pseudo 2gen";
		} else if (twoGen == 2) {
			std::cout << ", 2gen";
		}
		if (keepCubeShape) {
			std::cout << ", Stay in CS";
		}
		std::cout<< std::endl;
	}

	srand( (unsigned)time( NULL ) );
	char buffer[2000];
	do{
		clock_t now = clock();
		if( posArg<0 ){
			if( inpFile!=NULL ){
				is.getline(buffer,1999);
				int r=p.parseInput(buffer);
				if(r) {
					show(r);
					continue;
				}
			}else{
				p.random(twoGen, keepCubeShape);
			}
		}
		if( ignoreMid ) p.middle=0;

		// show position
		if(verbosity>=1){
			std::cout<<"State: ";
			p.print();
			std::cout<<std::endl;
		}

		// reject impossible params before initing the tables
		if (posArg < 0) {
			int pre = preValidate(p, keepCubeShape, twoGen);
			if (pre) { show(pre); continue; }
		}

		if (p.isPartial()) {
			// convert position to color encoding
			pps.set(p, findAll, ignoreTrans);

			// solve position
			int r = pps.solve(twoGen, extraMoves, keepCubeShape);
			if (r < 0) return 130;
			if (r) show(r);
		} else {
			// convert position to color encoding
			ps.set(p, findAll, ignoreTrans);

			//solve position
			int r = ps.solve(twoGen, extraMoves, keepCubeShape);
			if (r < 0) return 130;
			if (r) show(r);
		}

		if (verbosity>=6) std::cout << "Time: " << (clock() - now);
		std::cout<<std::endl;
	}while( posArg<0 && ( (inpFile!=NULL && !is.eof() ) || (inpFile==NULL && (numpos==0 || numpos-- > 1)) ));

	return(0);
}

/*
 * ===================== Batch Solver API =====================
 * Allows solving multiple positions with one pruning tables build
 */
namespace {
struct BatchState {
	ChoiceTable ct;
	ShapeTranTable st;
	ShpColTranTable scte;
	ShpColTranTable sctc;
	FullPosition q;
	PrunTable* pr1 = nullptr;
	PrunTable* pr2 = nullptr;
	CubePrunTable* cpr1 = nullptr;
	CubePrunTable* cpr2 = nullptr;
	PositionSolver* ps = nullptr;
	PartialPositionSolver* pps = nullptr;
	// shared cache for reusing dynamic pruning tables
	DynTableCache* dynCache = nullptr;
	bool keepCubeShape = false;
	bool ignoreMid = false;
	bool ignoreTrans = false;
	bool findAll = false;
	int twoGen = 0;
	int extraMoves = 0;
	bool initialized = false;

	BatchState() : scte(st, ct, true), sctc(st, ct, false) {}
	~BatchState() { destroy(); }

	void destroy() {
		initialized = false;
		// Solvers reference our tables, so delete them first.
		// PositionSolver has virtual methods but no virtual destructor,
		// so we delete through the exact derived type to avoid UB.
		delete pps; pps = nullptr;
		delete ps; ps = nullptr;
		delete dynCache; dynCache = nullptr;
		delete pr1; pr1 = nullptr;
		delete pr2; pr2 = nullptr;
		delete cpr1; cpr1 = nullptr;
		delete cpr2; cpr2 = nullptr;
	}
};

static BatchState* g_batch = nullptr;
}

static int batchParseFlags(int argc, char* argv[], BatchState& bs) {
	resetSolverOptions();
	bs.keepCubeShape = false;
	bs.ignoreMid = false;
	bs.ignoreTrans = false;
	bs.findAll = false;
	bs.twoGen = 0;
	bs.extraMoves = 0;
	usenegative = true;
	bool ignoreTransLocal = false;

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch (argv[i][1]) {
				case 'e': case 'E':
					if (argv[i][2]=='s'||argv[i][2]=='S') metric=SLICE_METRIC;
					else if (argv[i][2]=='m'||argv[i][2]=='M') metric=TURN_METRIC;
					else if (argv[i][2]=='a'||argv[i][2]=='A') metric=ANGLE_METRIC;
					else return 1;
					break;
				case 'x': ignoreTransLocal = true; break;
				case 'a': case 'A':
					bs.findAll = true;
					bs.extraMoves = parseInteger(argv[i]+2);
					if (bs.extraMoves < 0) return 15;
					break;
				case 'm': case 'M': bs.ignoreMid = true; break;
				case 'b': case 'B': usebrackets = true; break;
				case 'v': case 'V':
					verbosity = parseInteger(argv[i]+2);
					if (verbosity < 0) return 15;
					break;
				case 'g': case 'G': generator = true; break;
				case 'p': case 'P': bs.twoGen = 1; break;
				case '2': bs.twoGen = 2; break;
				case 'c': case 'C': bs.keepCubeShape = true; break;
				case 'k': case 'K':
					if (argv[i][2] == '0') karnotation = 0;
					else if (argv[i][2] == '2') karnotation = 2;
					else if (argv[i][2] == '3') karnotation = 3;
					else karnotation = 1;
					break;
				case 'n': case 'N':
					if (argv[i][2]=='b'||argv[i][2]=='B') { specificAngleTop=true; specificAngleBot=true; }
					else if (argv[i][2]=='u'||argv[i][2]=='U') specificAngleTop=true;
					else if (argv[i][2]=='d'||argv[i][2]=='D') specificAngleBot=true;
					else if (argv[i][2]=='n'||argv[i][2]=='N') { specificAngleTop=false; specificAngleBot=false; }
					else return 1;
					break;
				case 'X':
					{ int v = parseInteger(argv[i]+2); if (v >= 0 && v <= 6) maxX = v; }
					break;
				case 'Y':
					{ int v = parseInteger(argv[i]+2); if (v >= 0 && v <= 6) maxY = v; }
					break;
				case 'Z':
					{ int v = parseInteger(argv[i]+2); if (v >= 1 && v <= 12) maxTotal = v; }
					break;
				case 'd': case 'D':
					{
						std::string ds(argv[i]+2);
						std::stringstream ss(ds);
						std::string token;
						while(std::getline(ss, token, ',')) {
							token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
							if (!token.empty()) {
								try { specificDepths.push_back(std::stoi(token)); } catch(...) {}
							}
						}
					}
					break;
				default: return 1;
			}
		}
	}
	bs.ignoreTrans = ignoreTransLocal || (maxX != 6 || maxY != 6 || maxTotal != 12);
	return 0;
}

extern "C" int sq1_batch_init(int argc, char* argv[], const char* table_directory) {
	if (g_batch) { delete g_batch; g_batch = nullptr; }

	g_batch = new BatchState();
	sq1optSetTableDirectory(table_directory ? table_directory : ".");

	int err = batchParseFlags(argc, argv, *g_batch);
	if (err) { delete g_batch; g_batch = nullptr; return err; }

	if (verbosity >= 3) std::cout << "Batch: Initializing tables..." << std::endl;

	if (g_batch->keepCubeShape) {
		if (verbosity >= 4) std::cout << "  2. Computing restricted cubeshape pruning table #1" << std::endl;
		g_batch->cpr1 = new CubePrunTable(g_batch->q, 0, g_batch->st, g_batch->scte, g_batch->sctc);
		if (verbosity >= 4) std::cout << "  1. Computing restricted cubeshape pruning table #2" << std::endl;
		g_batch->cpr2 = new CubePrunTable(g_batch->q, 1, g_batch->st, g_batch->scte, g_batch->sctc);
	} else {
		if (verbosity >= 4) std::cout << "  2. Computing pruning table #1" << std::endl;
		g_batch->pr1 = new PrunTable(g_batch->q, 0, g_batch->st, g_batch->scte, g_batch->sctc);
		if (verbosity >= 4) std::cout << "  1. Computing pruning table #2" << std::endl;
		g_batch->pr2 = new PrunTable(g_batch->q, 1, g_batch->st, g_batch->scte, g_batch->sctc);
	}

	g_batch->ps = new PositionSolver(g_batch->st, g_batch->scte, g_batch->sctc, g_batch->pr1, g_batch->pr2, g_batch->cpr1, g_batch->cpr2);
	g_batch->pps = new PartialPositionSolver(g_batch->st, g_batch->scte, g_batch->sctc, g_batch->pr1, g_batch->pr2, g_batch->cpr1, g_batch->cpr2);
	// Cache dynamic pruning tables only in -c mode.
	// Metric doesn't change throughout session, which makes pruning tables valid.
	if (g_batch->keepCubeShape)
		g_batch->dynCache = new DynTableCache(300);
	g_batch->pps->dynCache = g_batch->dynCache;
	g_batch->initialized = true;

	if (verbosity >= 3) std::cout << "Batch: Tables ready." << std::endl;
	return 0;
}

extern "C" int sq1_batch_solve(const char* position) {
	if (!g_batch || !g_batch->initialized) return -1;
	if (!position || !position[0]) return 13;

	FullPosition p;
	int r = p.parseInput(position);
	if (r) { show(r); return r; }
	if (g_batch->ignoreMid) p.middle = 0;

	int pre = preValidate(p, g_batch->keepCubeShape, g_batch->twoGen);
	if (pre) { show(pre); return pre; }

	if (p.isPartial()) {
		g_batch->pps->set(p, g_batch->findAll, g_batch->ignoreTrans);
		r = g_batch->pps->solve(g_batch->twoGen, g_batch->extraMoves, g_batch->keepCubeShape);
	} else {
		g_batch->ps->set(p, g_batch->findAll, g_batch->ignoreTrans);
		r = g_batch->ps->solve(g_batch->twoGen, g_batch->extraMoves, g_batch->keepCubeShape);
	}
	return r;
}

extern "C" int sq1_batch_solve_multi(const char** candidates, int num_candidates) {
	if (!g_batch || !g_batch->initialized) return -1;
	if (!candidates || num_candidates <= 0) return -1;

	std::vector<bool> done(num_candidates, false);
	bool didDebugHeader = false;

	for (int depth = 0; depth <= maxTotal; depth++) {
		if (verbosity >= 2 && !didDebugHeader) {
			std::cerr << "batch_multi: " << num_candidates << " candidate(s):" << std::endl;
			for (int i = 0; i < num_candidates; i++)
				std::cerr << "  candidate[" << i << "] = \"" << (candidates[i] ? candidates[i] : "") << "\"" << std::endl;
			didDebugHeader = true;
		}
		specificDepths.clear();
		specificDepths.push_back(depth);

		bool allDone = true;
		for (int i = 0; i < num_candidates; i++) {
			if (done[i]) continue;
			allDone = false;

			if (stopRequested.load()) { specificDepths.clear(); return -1; }

			if (!candidates[i] || !candidates[i][0]) { done[i] = true; continue; }

			FullPosition p;
			int r = p.parseInput(candidates[i]);
			if (r) { if (verbosity >= 2) std::cerr << "  candidate[" << i << "] parse FAILED (code " << r << ")" << std::endl; done[i] = true; continue; }
			if (g_batch->ignoreMid) p.middle = 0;

			int pre = preValidate(p, g_batch->keepCubeShape, g_batch->twoGen);
			if (pre) { if (verbosity >= 2) std::cerr << "  candidate[" << i << "] preValidate FAILED (code " << pre << ")" << std::endl; done[i] = true; continue; }

			if (verbosity >= 2) std::cerr << "  depth " << depth << ": solving candidate[" << i << "] = \"" << candidates[i] << "\" (partial=" << p.isPartial() << ")" << std::endl;

			int solveResult;
			if (p.isPartial()) {
				g_batch->pps->set(p, false, g_batch->ignoreTrans);
				solveResult = g_batch->pps->solve(g_batch->twoGen, 0, g_batch->keepCubeShape);
			} else {
				g_batch->ps->set(p, false, g_batch->ignoreTrans);
				solveResult = g_batch->ps->solve(g_batch->twoGen, 0, g_batch->keepCubeShape);
			}

			if (solveResult < 0) { specificDepths.clear(); return solveResult; }
			if (solveResult != 0) { if (verbosity >= 2) std::cerr << "  candidate[" << i << "] no solution at depth " << depth << std::endl; done[i] = true; continue; }

			PositionSolver* solver = p.isPartial()
				? static_cast<PositionSolver*>(g_batch->pps)
				: static_cast<PositionSolver*>(g_batch->ps);
			if (solver->m_solutionFound) {
				if (verbosity >= 2) std::cerr << "  candidate[" << i << "] SOLVED at depth " << depth << std::endl;
				specificDepths.clear();
				return 0;
			}
			if (verbosity >= 2) std::cerr << "  candidate[" << i << "] completed depth " << depth << " (no solution found)" << std::endl;
		}

		if (allDone) break;
	}

	specificDepths.clear();
	return 0;
}

extern "C" void sq1_batch_destroy() {
	if (g_batch) { delete g_batch; g_batch = nullptr; }
}

// ===================== End Batch API =====================


#ifndef SQ1OPT_LIBRARY
int main(int argc, char* argv[])
{
	return sq1optMain(argc, argv);
}
#endif



/*
 * Precomputed tables (persisted to disk). Sizes:
 * ttshp  shape transitions           7356 shapes x 4 ints  (3 moves + E-mirror)
 * tt     color transitions           7356 x 70 x 3 chars   (one for edges, one for corners)
 * pt     pruning distances           7356 x 70 x 70 chars  (primary + mirror colorings)
*/
