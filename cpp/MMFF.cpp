
#include "macroUtils.h"

#include "fastmath.h"
#include "Vec2.h"
#include "Vec3.h"

static int  iDebug = 0;

#include "MMFF.h"
#include "NBFF.h"
#include "MMFFBuilder.h"
#include "MMFFBuilder.h"
#include "DynamicOpt.h"

using namespace MMFF;

// ========= data

ForceField   ff;        // inter moleculer forcefield
NBFF         nff;       // non-bonding intermolecular forcefield
Builder      builder;
DynamicOpt   opt;

bool bNonBonded = false;

//ForceField::iDebug =iDebug;
//NBFF      ::iDebug =iDebug;
//Builder   ::iDebug =iDebug;
//DynamicOpt::iDebug =iDebug;

struct{
    //                     R           eps     Q
    Vec3d  REQ = (Vec3d) { 1.4870,sqrt(0.000681),0};   // Hydrogen
    double bond_l0      = 1.5; 
    double bond_k       = 100/const_eVA2_Nm;   // 100 N/m -> eV/A^2
    double hydrogen_l0  = 1.07; 
    double hydrogen_k   = 100/const_eVA2_Nm;
    double angle_kpi    = 1.5;
    double angle_ksigma = 1.0;
} defaults;

// ========= Export Fucntions to Python

extern "C"{

double* getPos  (){ return (double*)ff.apos;   }
double* getForce(){ return (double*)ff.aforce; }

double setupOpt( double dt, double damp, double f_limit, double l_limit ){
    opt.initOpt( dt, damp );
    opt.f_limit = f_limit;
    opt.l_limit = l_limit;
}

void addAtoms( int n, double* pos_, int* npe_ ){
    Atom brushAtom{-1,-1,-1, Vec3dZero, defaults.REQ };
    Vec3d* pos = (Vec3d*)pos_;
    Vec2i* npe = (Vec2i*)npe_;
    for(int i=0;i<n;i++){
        //printf( "atom[%i] (%g,%g,%g) &npe_ %i \n", i, pos[i].x, pos[i].y, pos[i].z, npe_ );
        brushAtom.pos = pos[i];
        if( npe ){ builder.insertAtom(brushAtom, true )->setNonBond( npe[i].a, npe[i].b ); }
        else     { builder.insertAtom(brushAtom, false);  }   // atom without configuration set
    }
}

void addBonds( int n, int* bond2atom_, double* l0s, double* ks ){
    Bond brushBond{-1,  {-1,-1}, defaults.bond_l0, defaults.bond_k };
    Vec2i* bond2atom = (Vec2i*)bond2atom_;
    for(int i=0;i<n;i++){
        brushBond.atoms=bond2atom[i];
        if(l0s){ brushBond.l0 = l0s[i]; }
        if(ks ){ brushBond.k  = ks [i]; }
        //printf("[%i/%i] ",i,n);   println(brushBond); //brushBond.print();
        builder.insertBond(brushBond);
    }
}

/*
void addAngles(){

}

void addDihedrals(){
    MMFFDihedral brushDihedral{ -1,   -1,-1,-1,    3, 0.5 };
    println(brushDihedral);
    builder.insertDihedralByAtom( {0,1,2,3}, brushDihedral );
}
*/

void    setAllNonBonded ( double* REQs        ){ nff.bindOrRealloc( ff.natoms, ff.nbonds, ff.apos, ff.aforce, (Vec3d*)REQs, ff.bond2atom ); bNonBonded = true; }

double* setSomeNonBonded( int n, double* REQs ){ 
    if( !checkPairsSorted( ff.nbonds, ff.bond2atom ) ){ printf( "ERROR: ff.bonds is not sorted => exit \n" ); return 0; };
    nff.bindOrRealloc( ff.natoms, ff.nbonds, ff.apos, ff.aforce, 0, ff.bond2atom );
    for(int i=0; i<n; i++    ){ nff.REQs[i]=((Vec3d*)REQs)[i];       };
    for(int i=n; i<nff.n; i++){ nff.REQs[i]=defaults.REQ; };
    bNonBonded = true;
    return (double*)nff.REQs;
}

double* setNonBonded( int n, double* REQs){
    //printf( "setNonBonded REQs %li  %i<?%i \n", (long)REQs, n,ff.natoms );
    if(n<ff.natoms){ if(iDebug>0)printf( "realocate REQs\n" ); return setSomeNonBonded( n, REQs ); }
    else           { setAllNonBonded( REQs );                                  }
    return (double*)nff.REQs;
}

int buildFF( bool bAutoHydrogens, bool bAutoAngles, bool bSortBonds ){

    builder.capBond  = Bond{ -1, -1,-1, defaults.hydrogen_l0, defaults.hydrogen_k };        // C-H bond ?

    if( bAutoHydrogens){ builder.makeAllConfsSP(); }

    if( bSortBonds && ( !builder.checkBondsSorted() ) ){
        if( !builder.sortBonds() ){ printf( " ERROR in builder.sortBonds() => exit \n" ); return -1; }
    }

    if( bAutoAngles   ){ builder.autoAngles( defaults.angle_ksigma, defaults.angle_kpi ); }
    builder.toForceField( ff );

    opt.bindOrAlloc( 3*ff.natoms, (double*)ff.apos, 0, (double*)ff.aforce, 0 );
    //opt.setInvMass( 1.0 );
    opt.cleanVel( );

    return ff.natoms;
}

double relaxNsteps( int ialg, int nsteps, double Fconv ){
    double F2=1.0,E =0;
    double F2conv=Fconv*Fconv;
    for(int itr=0; itr<nsteps; itr++){
        E=0,F2=1;
        ff.cleanAtomForce();
        E += ff.eval();
        E += nff.evalLJQ_sortedMask();
        if(iDebug>0){
            Vec3d cog,fsum,torq;
            checkForceInvariatns( ff.natoms, ff.aforce, ff.apos, cog, fsum, torq );
            //printf( "DEBUG CHECK INVARIANTS  fsum %g torq %g   cog (%g,%g,%g) \n", fsum.norm(), torq.norm(), cog.x, cog.y, cog.z );
        }
        switch(ialg){
            case 0: F2 = opt.move_FIRE();  break;
            case 1: opt.move_GD(opt.dt);   break;
            case 3: opt.move_MD(opt.dt);   break;
        }
        //printf( "F2 %g F %g F2conv %g Fconv %g \n", F2, sqrt(F2), F2conv, Fconv );
        if(iDebug>0){ printf("relaxNsteps[%i] |F| %g Fconf %g E %g dt %g(%g..%g) damp %g %g \n", itr, sqrt(F2), Fconv, E, opt.dt, opt.dt_min, opt.dt_max, opt.damping, opt.f_limit ); }
        if(F2<F2conv) break;
    }
    return sqrt(F2);
}


}
