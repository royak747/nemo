/*
 *  SNAPMASS:  set masses in a snapshot
 *
 *	1-Aug-88	V1.0 created      				PJT
 *	5-dec-88	V1.1 allow all masses same through one keyword  PJT
 *				(proposed - to be coded )
 *	5-feb-89	V1.2 allow bodytrans(3NEMO) expression for mass PJT
 *	13-may-91	V1.3 added helpvec etc.				PJT
 *      23-nov-93       V1.3a cosmetic
 *      21-dec-95       V1.4 process all snapshots
 *	28-may-97	2.0 allow bodytrans and generic function	PJT
 *				(code taken from mkplummer)
 *      24-jul-97       2.1 added norm=
 *	 8-sep-01       a   init_xrandom
 *      29-aug-06       c   prototypes for frandom() and getrfunc() properly used
 *      12-jul-2019     2.2 added ccd=
 *      14-aug-2020     2.3 allow Aux to be used
 *       5-nov-2023     2.4 add timers
 *
 */
#include <stdinc.h>
#include <getparam.h>
#include <vectmath.h>
#include <filestruct.h>
#include <history.h>
#include <image.h>
#include <loadobj.h>

#include <snapshot/snapshot.h>
#include <snapshot/body.h>
#include <snapshot/get_snap.c>
#include <snapshot/put_snap.c>

#include <bodytrans.h>
#include <timers.h>

string defv[] = {
    "in=???\n		      input (snapshot) file",
    "out=???\n                output (snapshot) file",
    "mass=\n		      expression for new masses",
    "inmass=\n                extra file, if to use their masses only",
    "massname=\n              If used,  Mass-function name (mostly n(m))",
    "massexpr=pow(m,p)\n      Mass function expression (e.g. pow(m,p))",
    "masspars=p,0.0\n         Mass function parameters (e.g. p,0.0)",
    "massrange=1,1\n          Range for mass-spectrum (e.g. 1,2)",
    "seed=0\n                 Random seed",
    "ccd=\n                   Input CCD with mapvalues that represent mass",
    "norm=\n                  Normalization value for the total mass (if used)",
    "aux=f\n                  Store in Aux instead?",
    "VERSION=2.5a\n           30-jan-2025 PJT",
    NULL,
};

string usage="(re)assign masses to a snapshot";


extern real_proc getrfunc(string , string , string , int *);

void nemo_main(void)
{
    stream instr, inmassstr, outstr, ccdstr;
    real   tsnap, tsnapmass, mtot, mrange[2], norm;
    real   xpos, ypos, xmin, ymin, idx, idy;
    Body  *btab = NULL, *bp;
    Body  *bmasstab = NULL, *bmassp;
    imageptr iptr = NULL;
    int i, n, nbody, nbodymass, bits, bitsmass, seed, ix, iy;
    rproc_body  bfunc;
    real_proc   mfunc;
    bool  Qnorm, first = TRUE;
    bool Qaux = getbparam("aux");
    string what = (Qaux ? strdup("aux") : strdup("mass"));
    int itimers=0, ntimers = 100;

    init_timers(ntimers+1);
    stamp_timers(itimers++);                                                        // TIMERS

    instr = stropen(getparam("in"), "r");
    if (hasvalue("mass")) {
        dprintf(0,"Using a bodytrans %s expression\n", what);
        bfunc = btrtrans(getparam("mass"));     /* use bodytrans expression */
        mfunc = NULL;
        inmassstr = NULL;
    } else if (hasvalue("massname")) {
        dprintf(0,"Using a general functional mass expression\n");
        mysymbols(getargv0());
        n=1;
        mfunc = getrfunc(getparam("massname"),
                         getparam("massexpr"),getparam("masspars"),&n);
        if (nemoinpr(getparam("massrange"),mrange,2)!=2)
            error("Need two numbers in massrange=");
        dprintf(1,"Massrange from %f : %f\n",mrange[0],mrange[1]);
        bfunc = NULL;
        inmassstr = NULL;
    } else if (hasvalue("inmass")) {
        inmassstr = stropen(getparam("inmass"),"r");
    } else if (hasvalue("ccd")) {
      warning("new ccd mode");
      ccdstr = stropen(getparam("ccd"),"r");
      read_image(ccdstr,&iptr);
      idx = 1.0/Dx(iptr);
      idy = 1.0/Dy(iptr);
      xmin = Xmin(iptr);
      ymin = Ymin(iptr);
      inmassstr = NULL;
      mfunc = NULL;
      bfunc = NULL;      
    } else 
    	error("One of: mass=, massname=, inmass=, ccd= must be given");
    outstr = stropen(getparam("out"), "w");
    seed = init_xrandom(getparam("seed"));
    dprintf(1,"seed=%d\n",seed);
    Qnorm = hasvalue("norm");
    if (Qnorm) norm = getdparam("norm");

    for(;;) {
					/* input main data */
    	get_history(instr);                    /* skip history & comments */
        if (!get_tag_ok(instr, SnapShotTag)) {
            break;
            error("Snapmass (in): Need a snapshot");
        }
        get_snap(instr, &btab, &nbody, &tsnap, &bits);
        stamp_timers(itimers++);	                                                        // TIMERS	
        if (bits&MassBit) {
            dprintf(0,"Warning: existing masses overwritten\n");
            if (!Qnorm) {
            	Qnorm = TRUE;
        	for (bp=btab, i=0, norm=0.0; i<nbody; bp++,i++)            
            	    norm += Mass(bp);
            }
        }
        if (!(bits&TimeBit)) {
            dprintf(1,"Warning: time=0.0 assumed\n");
            tsnap = 0.0;
        }
        
        if (inmassstr) { 		/* input mass data from a different file */
    	    get_history(inmassstr);
            if (!get_tag_ok(inmassstr, SnapShotTag))
       		error("Snapmass (inmass): Need a snapshot");
            get_snap(inmassstr, &bmasstab, &nbodymass, &tsnapmass, &bitsmass);
            if (nbodymass != nbody) {
	        if (nbodymass < nbody)
                    error("too few bodies (%d < %d)",nbodymass,nbody);
                else
                    warning("too many bodies (%d > %d)",nbodymass,nbody);
            }
            if ((bitsmass & MassBit) == 0) {
  	        // unlink(getparam("out"));
                error("essential mass data missing\tbits = %x\n", bits);
            }
        }

        stamp_timers(itimers++);	                                                        // TIMERS
        				/* output */
        if (first) {
            put_history(outstr);
            first = FALSE;
        }
        if (inmassstr) {
	    if (nbody==nbodymass)
	        for (bp=btab, bmassp=bmasstab; bp<btab+nbody; bp++, bmassp++)
		  if (Qaux)
		    Aux(bp) = Mass(bmassp);
		  else
		    Mass(bp) = Mass(bmassp);
	    else
	        for (bp=btab, bmassp=bmasstab; bp<btab+nbody; bp++)
		  if (Qaux)
	            Aux(bp) = Mass(bmassp);
		  else
	            Mass(bp) = Mass(bmassp);
        } else if (bfunc) {
            for (bp=btab, i=0; i<nbody; bp++,i++)
	      if (Qaux)
                Aux(bp) = bfunc(bp, tsnap, i);
	      else
                Mass(bp) = bfunc(bp, tsnap, i);
        } else if (mfunc) {
            for (bp=btab, i=0; i<nbody; bp++,i++)
	      if (Qaux)
	        Aux(bp) = frandom(mrange[0], mrange[1], mfunc);
	      else
	        Mass(bp) = frandom(mrange[0], mrange[1], mfunc);
	} else if (iptr) {
	    for (bp=btab, i=0; i<nbody; bp++,i++) {
	      xpos = (Pos(bp)[0]-xmin)*idx + 0.5;    /* fractional cell index   0..nx */
	      ypos = (Pos(bp)[1]-ymin)*idy + 0.5;
	      ix = (int) floor(xpos);            /* cell index:   0 .. nx-1 */
	      iy = (int) floor(ypos);
	      if (ix<0 || iy<0 || ix>=Nx(iptr) || iy>=Ny(iptr))
		if (Qaux)
      		  Aux(bp) = 0.0;
		else
      		  Mass(bp) = 0.0;
	      else
		if (Qaux)
		  Aux(bp) = MapValue(iptr,ix,iy);
		else
		  Mass(bp) = MapValue(iptr,ix,iy);
	    }
        } else             
            error("bad flow logic");

        for (bp=btab, i=0, mtot=0.0; i<nbody; bp++,i++)            
	  mtot += (Qaux ? Aux(bp) : Mass(bp));
        dprintf(0,"Total %s: %g\n", what , mtot);
        if (Qnorm) {
	  dprintf(0,"Rescaling total %s to %g\n", what , norm);
            for (bp=btab, i=0; i<nbody; bp++,i++)
	      if (Qaux)
                Aux(bp) *= norm/mtot;
	      else
                Mass(bp) *= norm/mtot;
        }
	bits |= MassBit;    /* turn mass bit on anyways */
	if (Qaux)
	  bits |= AuxBit;
        stamp_timers(itimers++);	                                            // TIMERS
        put_snap(outstr, &btab, &nbody, &tsnap, &bits);
    }
    stamp_timers(itimers++);                                                        // TIMERS
    strclose(instr);
    if (inmassstr) strclose(inmassstr);
    strclose(outstr);
    stamp_timers(itimers++);                                                        // TIMERS
    for (i=0; i<itimers; i++)
      dprintf(1,"timers[%d]: %Ld0\n",i,diff_timers(i,i+1));
    dprintf(1,"timersSUM: %Ld0\n",diff_timers(0,itimers));    
    

}
