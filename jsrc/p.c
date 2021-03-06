/* Copyright 1990-2008, Jsoftware Inc.  All rights reserved.               */
/* Licensed use only. Any other use is in violation of copyright.          */
/*                                                                         */
/* Parsing; see APL Dictionary, pp. 12-13 & 38.                            */

#include "j.h"
#include "p.h"
#include <stdint.h>

#define RECURSIVERESULTSCHECK   // obsolete
//  if(y&&(AT(y)&NOUN)&&!(AFLAG(y)&AFVIRTUAL)&&((AT(y)^AFLAG(y))&RECURSIBLE))SEGFAULT;  // stop if nonrecursive noun result detected


#define PARSERSTKALLO (490*sizeof(PSTK))  // number of stack entries to allocate, when we allocate, in bytes

/* parsing benchmark

parsing & name lookup
9!:5 :: 0: 1
namelkp =: 3 : 0
totl =. 0.
for_i. y do. totl =. totl + 1. + 2. + (+/ % #) i end.
totl
)
9!:5 :: 0: 0
6!:2 'namelkp"2 i. 100 10000 4'

parsing only
namelkp =: 3 : 0
totl =. 0. [ a =. 2. [ ab =. 3.
for. y do.
 totl =. totl + +: 1. - +: a * ab + 1.
 totl =. * totl - ab + - a
 totl =. totl - 2.
end.
totl
)
(10) 6!:2 'namelkp i. 1e6'
namelkp =: 3 : 0
for. y do.
y
end.
0
)
(10) 6!:2 'namelkp i. 1e6'

namelkp =: 3 : 0
for. y do.
end.
0
)
(10) 6!:2 'namelkp i. 1e6'

*/
/* NVR - named value reference                                          */
/* a value referenced in the parser which is the value of a name        */
/* (that is, in some symbol table).                                     */
/*                                                                      */
// jt->nvra      A block for NVR stack
// AAV1(jt->nvra)        the stack.  LSB in each is a flag
// AN(jt->nvra)  size of stack
/*                                                                      */
/* Each call of the parser records the current NVR stack top (nvrtop),  */
/* and pop stuff off the stack back to that top on exit       */
/*                                                                      */
// The nvr stack contains pointers to values, added as names are moved
// from the queue to the stack.  Local values are not pushed.

B jtparseinit(JS jjt, I nthreads){A x;
 I threadno; for(threadno=0;threadno<nthreads;++threadno){JJ jt=&jjt->threaddata[threadno];
  GAT0(x,INT,20,1); ACINIT(x,ACUC1) jt->nvra=x;   // Initial stack.  Size is doubled as needed
  // ras not required because this is called during initialization 
 }
 R 1;
}

// w is a block that looks ripe for in-place assignment.  We just have to make sure that it is not in use somewhere up the stack.
// It isn't, if (1) it isn't on the stack at all; (2) if it was put on the stack by the currently-executing sentence.  We call this
// routine only when we are checking inplacing for final assignments, or for virtual extension.  For final assignment the parser stack is guaranteed to be empty; so any
// use of the name that was called for by this sentence must be finished
I jtnotonupperstack(J jt, A w) {
 // w is known nonzero.  In-place assign it only if its NVR count is 1, indicating that only one sentence may have this name stacked
 // This test is not locked because if we inplace we have a race condition anyway; so we have to have higher-level means to avoid that
 if(likely((AM(w)&~AMFREED)==AMNV+AMNVRCT*1)){
  // see if name was stacked (for the only time) in this very sentence
  A *v=jt->parserstackframe.nvrotop+AAV1(jt->nvra);  // point to current-sentence region of the nvr area
  DQ(jt->parserstackframe.nvrtop-jt->parserstackframe.nvrotop, if(*v==w)R 1; ++v;);   // if name stacked in this sentence, that's OK
 }
 // not stacked here, so not reassignable here.  see if name was not stacked at all - that would be OK
 R !(AM(w)&(-(AM(w)&AMNV)<<1));   // return OK if name not using NV semantics or stackcount=0 (rare, since why did we think is might be inplacable?)
}


#define AVN   (     ADV+VERB+NOUN)
#define CAVN  (CONJ+ADV+VERB+NOUN)
#define EDGE  (MARK+ASGN+LPAR)

PT cases[] = {
 EDGE,      VERB,      NOUN, ANY,       0,  jtvmonad, 1,2,1,
 EDGE+AVN,  VERB,      VERB, NOUN,      0,  jtvmonad, 2,3,2,
 EDGE+AVN,  NOUN,      VERB, NOUN,      0,    jtvdyad,  1,3,2,
 EDGE+AVN,  VERB+NOUN, ADV,  ANY,       0,     jtvadv,   1,2,1,
 EDGE+AVN,  VERB+NOUN, CONJ, VERB+NOUN, 0,    jtvconj,  1,3,1,
 EDGE+AVN,  VERB+NOUN, VERB, VERB,      0, jtvfolk,  1,3,1,
 EDGE,      CAVN,      CAVN, ANY,       0,  jtvhook,  1,2,1,
 NAME+NOUN, ASGN,      CAVN, ANY,       0,      jtvis,    0,2,1,
 LPAR,      CAVN,      RPAR, ANY,       0,    jtvpunc,  0,2,0,
};
#define PN 0
#define PA 1
#define PC 2
#define PV 3
#define PM 4  // MARK
#define PNM 5  // NAME
#define PL 6
#define PR 7
#define PS 8  // ASGN without ASGNNAME
#define PSN 9 // ASGN+ASGNNAME
#define PX 255

// Tables to convert parsing type to mask of matching parse-table rows for each of the stack positions
// the AND of these gives the matched row (the end-of-table row is always matched)
// static const US ptcol[4][10] = {
//     PN     PA     PC     PV     PM     PNM    PL     PR     PS     PSN
// { 0x2BE, 0x23E, 0x200, 0x23E, 0x27F, 0x280, 0x37F, 0x200, 0x27F, 0x27F},
// { 0x37C, 0x340, 0x340, 0x37B, 0x200, 0x200, 0x200, 0x200, 0x280, 0x280},
// { 0x2C1, 0x2C8, 0x2D0, 0x2E6, 0x200, 0x200, 0x200, 0x300, 0x200, 0x200},
// { 0x3DF, 0x3C9, 0x3C9, 0x3F9, 0x3C9, 0x3C9, 0x3C9, 0x3C9, 0x3C9, 0x3C9},
// };
// Remove bits 8-9
// Distinguish PSN from PS by not having PSN in stack[3] support line 0 (OK since it must be preceded by NAME and thus will run line 7)
// Put something distictive into LPAR that can be used to create line 8
#define PTNOUN 0xDFC17CBE
#define PTMARK 0xC900007F
static const UI4 ptcol[11] = {  // there is a gap at SYMB.  CONW is used to hold ASGNNAME.  
[LASTNOUNX-LASTNOUNX] = PTNOUN,  // PN
[ASGNX-LASTNOUNX] = 0xC900807F,  // PS
[MARKX-LASTNOUNX] = PTMARK,  // PM
[NAMEX-LASTNOUNX] = 0xC9000080,  // PNM
[SYMBX-LASTNOUNX] = 0xC800807F,  // PS+NAME
// gap [CONWX-LASTNOUNX]
[LPARX-LASTNOUNX] = 0x0100007F,  // PL
[VERBX-LASTNOUNX] = 0xF9E67B3E,  // PV
[ADVX-LASTNOUNX] = 0xC9C8403E,  // PA
[CONJX-LASTNOUNX] = 0xC9D04000,  // PC
[RPARX-LASTNOUNX] = 0xC9000000  // PR
};


// tests for pt types
// in pt0ecam, bits 16-21 and 23 of pt0 are used to hold the type flags read from the symbol, when names are processed
#define PTTYPEFLAGX 16  // VALTYPEMASK<<PTTYPEFLAGX is filled in the the type of the resolved name
#define PTISCAVNX 22  // this flag used in a register here
#define PTISCAVN(pt) ((pt)&(1LL<<PTISCAVNX))
#define PTISRPAR0(pt) ((pt)&0x7fff)
#define PTISM(s)  ((s).pt==PTMARK)
#define PTOKEND(t2,t3) (((PTISCAVN(~(t2).pt))+((t3).pt^PTMARK))==0)  // t2 is CAVN and t3 is MARK
#define PTNAMEIFASGN(pt)  ((pt)<<(NAMEX-15))   // we compare against the NAMEX bit
#define PTISNOTASGNNAME(pt)  ((pt)&0x1000000)
#define PTNOTLPARX 27  // this bit is set for NOT LPAR    used in a register here
#define PTNOTLPAR (1LL<<PTNOTLPARX)  // this bit is set in pt only if NOT LPAR
// converting type field to pt, store in z
#define PTFROMTYPE(z,t) {I pt=CTTZ(t); pt=(t)&(((1LL<<(LASTNOUNX+1))-1))?LASTNOUNX:pt; z=ptcol[pt-LASTNOUNX];}  // here when we know it's CAVN (not assignment)
#define PTFROMTYPEASGN(z,t) {I pt=CTTZ(t); I nt=LASTNOUNX; nt=(t)&CONW?SYMBX:nt; pt=(t)&(CONW|((1LL<<(LASTNOUNX+1))-1))?nt:pt; z=ptcol[pt-LASTNOUNX];}  // clear flag bit if ASGN to name, by fetching from unused SYMB hole (use SYMB rather than CONW because of odd code generation)

static PSTK* jtpfork(J jt,PSTK *stack){
 A y=folk(stack[1].a,stack[2].a,stack[3].a);  // create the fork
RECURSIVERESULTSCHECK
 RZ(y);  // if error, return 0 stackpointer
 stack[3].t = stack[1].t; stack[3].a = y;  // take err tok from f; save result; no need to set parsertype, since it didn't change
 stack[2]=stack[0]; R stack+2;  // close up stack & return
}

static PSTK* jtphook(J jt,PSTK *stack){
 A y=hook(stack[1].a,stack[2].a);  // create the hook
RECURSIVERESULTSCHECK
 RZ(y);  // if error, return 0 stackpointer
 PTFROMTYPE(stack[2].pt,AT(y)) stack[2].t = stack[1].t; stack[2].a = y;  // take err tok from f; save result.  Must store new type because this line takes adverb hooks also
 stack[1]=stack[0]; R stack+1;  // close up stack & return
}

// multiple assignment not to constant names.  self has parms.  ABACK(self) is the symbol table to assign to, valencefns[0] is preconditioning routine to open value or convert it to AR
static DF2(jtisf){RZ(symbis(onm(a),CALL1(FAV(self)->valencefns[0],w,0L),ABACK(self))); R num(0);} 

// assignment, single or multiple
// return sets stack[0].t to -1 if this is a final assignment    scaf should pass in m to use for detecting final assignment
static PSTK* jtis(J jt,PSTK *stack){
 I asgt=AT(stack[1].a); A v=stack[2].a, n=stack[0].a;  // assignment type, value and name
 J jtinplace=(J)((I)jt+((stack[0].t==1)<<JTFINALASGNX));   // set JTFINALASGN if this is final assignment
 stack[1+(stack[0].t==1)].t=-1;  // if the word number of the lhs is 1, it's either (noun)=: or name=: or 'value'=: at the beginning of the line; set token#=-1 to suppress display.  If not, make harmless store to slot 1
 if(likely(jt->asginfo.assignsym!=0)){jtsymbis(jtinplace,n,v,(A)asgt);}   // Assign to the known name.  Pass in the type of the ASGN
 else {B ger=0;C *s;
  // Point to the block for the assignment; fetch the assignment pseudochar (=. or =:); choose the starting symbol table
  // depending on which type of assignment (but if there is no local symbol table, always use the global)
  A symtab=jt->locsyms; if(unlikely((SGNIF(asgt,ASGNLOCALX)&(1-AN(jt->locsyms)))>=0))symtab=jt->global;
  if(unlikely(AT(n)==BOX+BOXMULTIASSIGN)){   // test both bits, since BOXMULTIASSIGN has multiple uses
   // string assignment, where the NAME blocks have already been computed.  Use them.  The fast case is where we are assigning a boxed list
   if(AN(n)==1)n=AAV(n)[0];  // if there is only 1 name, treat this like simple assignment to first box, fall through
   else{
    // True multiple assignment
    ASSERT((-(AR(v))&(-(AN(n)^AS(v)[0])))>=0,EVLENGTH);   // v is atom, or length matches n
    if(((AR(v)^1)+(~AT(v)&BOX))==0){A *nv=AAV(n), *vv=AAV(v); DO(AN(n), jtsymbis(jtinplace,nv[i],vv[i],symtab);)}  // v is boxed list
    else {A *nv=AAV(n); DO(AN(n), jtsymbis(jtinplace,nv[i],ope(AR(v)?from(sc(i),v):v),symtab);)}  // repeat atomic v for each name, otherwise select item.  Open in either case
    goto retstack;
   }
  }
  // single assignment or variable assignment
  if(unlikely((SGNIF(AT(n),LITX)&(AR(n)-2))<0)){
   // lhs is ASCII characters, atom or list.  Convert it to words
   s=CAV(n); ger=CGRAVEC==s[0];   // s->1st character; remember if it is `
   RZ(n=words(ger?str(AN(n)-1,1+s):n));  // convert to words (discarding leading ` if present)
   ASSERT(AN(n)||(AR(v)&&!AS(v)[0]),EVILNAME);  // error if namelist empty or multiple assignment with no values, if there is something to be assigned
   if(1==AN(n)){
    // Only one name in the list.  If one-name AR assignment, leave as a list so we go through the AR-assignment path below
    if(!ger){RZ(n=head(n));}   // One-name normal assignment: make it a scalar, so we go through the name-assignment path & avoid unboxing
   }
  }
  // if simple assignment to a name (normal case), do it
  if(likely((NAME&AT(n))!=0)){
#if FORCEVIRTUALINPUTS
   // When forcing everything virtual, there is a problem with jtcasev, which converts its sentence to an inplace special.
   // The problem is that when the result is set to virtual, its backer does not appear in the NVR stack, and when the reassignment is
   // made the virtual block is dangling.  The workaround is to replace the block on the stack with the final value that was assigned:
   // not allowed in general because of (verb1 x verb2) name =: virtual - if verb2 assigns the name, the value going into verb1 will be freed before use
   stack[2].a=
#endif
   jtsymbis(jtinplace,n,v,symtab);
  }else{
   // computed name(s)
   ASSERT(AN(n)||(AR(v)&&!AS(v)[0]),EVILNAME);  // error if namelist empty or multiple assignment to no names, if there is something to be assigned
   // otherwise, if it's an assignment to an atomic computed name, convert the string to a name and do the single assignment
   if(!AR(n))jtsymbis(jtinplace,onm(n),v,symtab);
   else {
    // otherwise it's multiple assignment (could have just 1 name to assign, if it is AR assignment).
    // Verify rank 1.  For each lhs-rhs pair, do the assignment (in jtisf).
    // if it is AR assignment, apply jtfxx to each assignand, to convert AR to internal form
    // if not AR assignment, just open each box of rhs and assign
    ASSERT(1==AR(n),EVRANK); ASSERT(AT(v)&NOUN,EVDOMAIN);
    // create faux fs to pass args to the multiple-assignment function, in AM and valencefns
    PRIM asgfs; ABACK((A)&asgfs)=symtab; FAV((A)&asgfs)->flag2=0; FAV((A)&asgfs)->valencefns[0]=ger?jtfxx:jtope;   // pass in the symtab to assign, and whether w must be converted from AR.  flag2 must be 0 to satisfy rank2ex
    I rr=AR(v)-1; rr&=~REPSGN(rr); rank2ex(n,v,(A)&asgfs,0,rr,0,rr,jtisf);
   }
  }
 }
retstack:  // return, but 0 if error
 stack+=2; if(unlikely(jt->jerr))stack=0; R stack;  // the result is the same value that was assigned
}


#if AUDITEXECRESULTS
// go through a block to make sure that the descendants of a recursive block are all recursive, and that no descendant is virtual/unincorpable
// and that any block marked PRISTINE, if boxed, has DIRECT descendants with usecount 1
// Initial call has nonrecurok and virtok both set

void auditblock(J jt,A w, I nonrecurok, I virtok) {
 if(!w)R;
 if(AC(w)<0&&AZAPLOC(w)==0)SEGFAULT;
// if(AC(w)<0&&!(AFLAG(w)&AFVIRTUAL)&&AZAPLOC(w)>=jt->tnextpushp)SEGFAULT;  // requires large NTSTACK
 if(AC(w)<0&&!(AFLAG(w)&AFVIRTUAL)&&((I)AZAPLOC(w)<0x100000||(*AZAPLOC(w)!=0&&*AZAPLOC(w)!=w)))SEGFAULT;  // if no zaploc for inplaceable block, error
 I nonrecur = (AT(w)&RECURSIBLE) && ((AT(w)^AFLAG(w))&RECURSIBLE);  // recursible type, but not marked recursive
 if(AFLAG(w)&AFVIRTUAL && !(AFLAG(w)&AFUNINCORPABLE))if(AFLAG(ABACK(w))&AFVIRTUAL)SEGFAULT;  // make sure e real backer is valid and not virtual
 if(nonrecur&&!nonrecurok)SEGFAULT;
 if(AFLAG(w)&(AFVIRTUAL|AFUNINCORPABLE)&&!virtok)SEGFAULT;
 if(AT(w)==(I)0xdeadbeefdeadbeef)SEGFAULT;
 switch(CTTZ(AT(w))){
  case RATX:  
   {A*v=AAV(w); DO(2*AN(w), if(v[i])if(!(((AT(v[i])&NOUN)==INT) && !(AFLAG(v[i])&AFVIRTUAL)))SEGFAULT;);} break;
  case XNUMX:
   {A*v=AAV(w); DO(AN(w), if(v[i])if(!(((AT(v[i])&NOUN)==INT) && !(AFLAG(v[i])&AFVIRTUAL)))SEGFAULT;);} break;
  case BOXX:
   if(!(AFLAG(w)&AFNJA)){A*wv=AAV(w);
   DO(AN(w), if(wv[i]&&(AC(wv[i])<0))SEGFAULT;)
   I acbias=(AFLAG(w)&BOX)!=0;  // subtract 1 if recursive
   if(AFLAG(w)&AFPRISTINE){DO(AN(w), if(!((AT(wv[i])&DIRECT)>0))SEGFAULT;)}  // wv[i]&&(AC(w)-acbias)>1|| can't because other uses may be not deleted yet
   {DO(AN(w), auditblock(jt,wv[i],nonrecur,0););}
   }
   break;
  case VERBX: case ADVX:  case CONJX: 
   {V*v=VAV(w); auditblock(jt,v->fgh[0],nonrecur,0);
    auditblock(jt,v->fgh[1],nonrecur,0);
    auditblock(jt,v->fgh[2],nonrecur,0);} break;
  case B01X: case INTX: case FLX: case CMPXX: case LITX: case C2TX: case C4TX: case SBTX: case NAMEX: case SYMBX: case CONWX:
   if(ISSPARSE(AT(w))){P*v=PAV(w);  A x;
    if(!scheck(w))SEGFAULT;
    x = SPA(v,a); if(!(AT(x)&DIRECT))SEGFAULT; x = SPA(v,e); if(!((AT(x)&DIRECT)>0))SEGFAULT; x = SPA(v,i); if(!(AT(x)&DIRECT))SEGFAULT; x = SPA(v,x); if(!(AT(x)&DIRECT))SEGFAULT;
    auditblock(jt,SPA(v,a),nonrecur,0); auditblock(jt,SPA(v,e),nonrecur,0); auditblock(jt,SPA(v,i),nonrecur,0); auditblock(jt,SPA(v,x),nonrecur,0);
   }else if(NOUN & (AT(w) ^ (AT(w) & -AT(w))))SEGFAULT;
   break;
  case ASGNX: break;
  default: break; SEGFAULT;
 }
}
#endif




// Run parser, creating a new debug frame.  Explicit defs, which make other tests first, then go through jtparsea
// the result has bit 0 set if final assignment
// JT flags indicate whether call comes from ".
F1(jtparse){F1PREFIP;A z;
 ARGCHK1(w);
 A *queue=AAV(w); I m=AN(w);   // addr and length of sentence
 RZ(deba(DCPARSE,queue,(A)m,0L));  // We don't need a new stack frame if there is one already and debug is off
 z=jtparsea(jtinplace,queue,m);
 debz();
 R z;
}


#if FORCEVIRTUALINPUTS
// For wringing out places where virtual blocks are incorporated into results, we make virtual blocks show up all over
// any noun block that is not in-placeable and enabled for inplacing in jt will be replaced by a virtual block.  Then the audit of the
// result will catch any virtual blocks that slipped through into an incorporating entity.  sparse blocks cannot be virtualized.
// if ipok is set, inplaceable blocks WILL NOT be virtualized
A virtifnonip(J jt, I ipok, A buf) {
 RZ(buf);
 if(AT(buf)&NOUN && !(ipok && ACIPISOK(buf)) && !ISSPARSE(AT(buf)) && !(AFLAG(buf)&(AFNJA))) {A oldbuf=buf;
  buf=virtual(buf,0,AR(buf)); if(!buf && jt->jerr!=EVATTN && jt->jerr!=EVBREAK)SEGFAULT;  // replace non-inplaceable w with virtual block; shouldn't fail except for break testing
  I* RESTRICT s=AS(buf); I* RESTRICT os=AS(oldbuf); DO(AR(oldbuf), s[i]=os[i];);  // shape of virtual matches shape of w except for #items
    AN(buf)=AN(oldbuf);  // install # atoms
 }
 R buf;
}

// We intercept all the function calls, for this file only
static A virtdfs1(J jtip, A w, A self){
 J jt = (J)(intptr_t)((I)jtip&-4);  // estab legit jt
 w = virtifnonip(jt,(I)jtip&JTINPLACEW,w);
 R jtdfs1(jtip,w,self);
}
static A virtdfs2(J jtip, A a, A w, A self){
 J jt = (J)(intptr_t)((I)jtip&-4);  // estab legit jt
 a = virtifnonip(jt,(I)jtip&JTINPLACEA,a);
 w = virtifnonip(jt,(I)jtip&JTINPLACEW,w);
 R jtdfs2(jtip,a,w,self);
}
static A virtfolk(J jtip, A f, A g, A h){
 J jt = (J)(intptr_t)((I)jtip&-4);  // estab legit jt
 f = virtifnonip(jt,0,f);
 g = virtifnonip(jt,0,g);
 h = virtifnonip(jt,0,h);
 R jtfolk(jtip,f,g,h);
}
static A virthook(J jtip, A f, A g){
 J jt = (J)(intptr_t)((I)jtip&-4);  // estab legit jt
 f = virtifnonip(jt,0,f);
 g = virtifnonip(jt,0,g);
 R jthook(jtip,f,g);
}

// redefine the names for when they are used below
#define jtdfs1 virtdfs1
#define jtdfs2 virtdfs2
#define jtfolk virtfolk
#define jthook virthook
#endif

// name:: delete the symbol name but not deleting the value.  If the usecount of the value is 1 and it is not on the NVR stack, make it inplaceable.  Replace the nameref with a tpush or NVR free
#define USEDGLOBALX 21
#define USEDGLOBAL (1LL<<USEDGLOBALX)
static A namecoco(J jt, A y, I pt0ecam, L *s){F1PREFIP; A sv=s->val;
 if(((I)jtinplace&JTFROMEXEC))R sv;
 LX *locbuckets=LXAV0(jt->locsyms); L *sympv=JT(jt,sympv);
 A fndst=UNLXAV0(locbuckets); if(unlikely((pt0ecam&USEDGLOBAL)!=0))fndst=syrdforlocale(y);  // get locale to use.  This re-looks up global names, but they should be rare in name::
 LX *asymx=LXAV0(fndst)+SYMHASH(NAV(s->name)->hash,AN(fndst)-SYMLINFOSIZE);  // get pointer to index of start of chain; address of previous symbol in chain
 LX nextsymx=*asymx;  // symbol number pointed to by asymx, possibly w/permanent indicator
 while(sympv+SYMNEXT(nextsymx)!=s){asymx=&(sympv+SYMNEXT(nextsymx))->next; nextsymx=*asymx;}
 // asymx points to the chain field that points to s.  Bend the chain around s to delete symbol; clear val to delete name
 if(unlikely(s->flag&LCACHED)){*asymx=s->next;  // cached ref: remove its name, making it unmoored.  This ref can't make such a reference but a different ref could
 }else{
  s->val=0; s->valtype=0; // remove the value ref, but don't touch the value itself
  if(likely(!SYMNEXTISPERM(nextsymx))){*asymx=s->next; fa(s->name); s->name=0; s->flag=0; s->sn=0; s->next=sympv[0].next; sympv[0].next=nextsymx;}
 }
 // the name is deleted, leaving the value.  Make the value inplaceable if there are no refs out against it.  If it is a local variable we think any outstanding
 // inplaceable refs will last till the end of the sentence, where we tpop the value
 if(likely((AM(sv)&-AMNVRCT)==0)){
  // value is not on NVR stack.  Schedule a deletion for the end of the sentence; if this name is the ONLY reference to the value,
  // make the value inplaceable (and point AM to the death warrant to allow early free)
  // The difference between this code and jtis() is that we defer the free using the tpop stack rather than the NVR stack
  if(likely(AC(sv)==ACUC1)){ACSET(sv,ACINPLACE+ACUC1); AZAPLOC(sv)=jt->tnextpushp;}
  tpushnr(sv);  // install death warrant for this block after the sentence completes
 }else{
  // value is on NVR stack.  We cannot free it here or even at the end of the sentence.  Mark a deferred free. but if there is already a deferred free, just fa()
  if(AM(sv)&AMFREED){fa(sv)}else{AMNVROR(sv,AMFREED)}
 }
 R sv;  // always return sv to help register allocation.  Any error comes from tpush, where it is fatal
}

#define FP goto failparse;   // indicate parse failure and exit
#define EP goto exitparse;   // exit parser, preserving current status
#define EPZ(x) if(unlikely(!(x))){FP}   // exit parser if x==0

#if 0  // keep for commentary
// An in-place operation requires an inplaceable argument, which is marked as AC<0,
// Blocks are born non-inplaceable; they need to be marked inplaceable by the creator.
// Blocks that are assigned are marked not-inplaceable.
// But an in-placeable argument is not enough;
// The key point is that an in-place operation is connected to a FUNCTION CALL as well as a DATA BLOCK.
// For example, in (+/ % #) <: y the result of <: y has a usecount of 1 and is
// not used anywhere else, so it is passed into the fork as an inplaceable argument.
// This same block is not inplaceable when it is passed into # (because it will be needed later by +/)
// but it can be inplaceable when passed into +/.
//
// By setting the inplaceable flag in a result, a verb warrants that the block does not contain and is not contained in
// any other block.
//
// The 2 LSBs of jt are set to indicate inplaceability of arguments.  The caller sets them when e
// has no further use for the argument AND e knows that the callee can handle in-place arguments.
// An argument is inplaceable only if it is marked as such in the block AND in jt.
// A caller should set the bit in jt only if it knows that the argument is inplaceable, which
// will be true if (1) the block was created by the caller; or (2) the block was an argument to the caller with jt
// marked to indicate its inplaceability
//
// Bit 0 of jt is for w, bit 1 for a.
//
// There is one more piece to the inplace system: reassigned names.  When there is an
// assignment to a name, the block being reassigned can be reused for the output if:
//   the usecount is 1
//   the current execution is the only thing on the stack
//   the name can be resolved before the execution
// Resolving the name is vexed, because the execution might change the current locale, or the value
// of an indirect locative.  Moreover, the global name may be on the stack in higher stack frames,
// and assigning the name would change those values before they are executed.  Safest, therefore,
// would be to detect in-place assignment to local names only; but that would not support name =: name , blah
// which currently executes in-place (though with the aliasing problem mentioned above).
//
// The first rule is to have no truck with inplacing unless the execution is known to be locative-safe,
// i. e. will not change locale, path, or any name that might go into a locative.  This is marked by a
// flag ASGSAFE in the verb.

// Any assignment to a name is resolved to an address when the copula is encountered and there
//  is only one execution on the stack.  This resolution will always succeed for a local assignment to a name.
//  For a global assignment to a locative, it may fail, or may resolve to an address that is different from
//  the correct address after the execution.  The address of the L block for the symbol to be assigned is stored in jt->asginfo.assignsym.
//
// [As a time-saving maneuver, we store jt->asginfo.assignsym even if the name is not in-placeable because of its type or usecount.
// We can use jt->asginfo.assignsym to avoid re-looking-up the name.]
//
// If jt->asginfo.assignsym is set, the (necessarily inplaceable) verb may choose to perform an in-place
// operation.  It will check usecounts and addresses to decide whether to do this, and it bears the responsibility
// of worrying about names on the stack.  Note that local names are not put onto the stack, so absence of AFNVR suffices for them.
#endif
// extend NVR stack, returning the A block for it.  stack error on fail, since that's the likely cause
A jtextnvr(J jt){ASSERT(jt->parserstackframe.nvrtop<32000,EVSTACK); RZ(jt->nvra = ext(1, jt->nvra));  R jt->nvra;}

#define BACKMARKS 3   // amount of space to leave for marks at the end.  Because we stack 3 words before we start to parse, we will
 // never see 4 marks on the stack - the most we can have is 1 value + 3 marks.
#define FRONTMARKS 1  // amount of space to leave for front-of-string mark
// Parse a J sentence.  Input is the queue of tokens
// Result has PARSERASGNX (bit 0) set if the last thing is an assignment
// JT flag is used to indicate execution from ". - we can't honor name:: then, or perhaps some assignments
A jtparsea(J jt, A *queue, I nwds){F1PREFIP;PSTK * stack;A z,*v;
 // jt->parsercurrtok must be set before executing anything that might fail; it holds the original
 // word number+1 of the token that failed.  jt->parsercurrtok is set before dispatching an action routine,
 // so that the information is available for formatting an error display
  // Save info for error typeout.  We save sentence info once, and token info for every executed fragment
 PFRAME oframe=jt->parserstackframe;   // save all the stack status
 jt->parserstackframe.parserqueue=queue; jt->parserstackframe.parserqueuelen=(US)nwds;  // addr & length of words being parsed
 if(likely(nwds>1)) {  // normal case where there is a fragment to parse
  // As names are dereferenced, they are added to the nvr queue.  To save time in the loop, we now
  // make sure there is enough room in the nvr queue to handle all the names we will encounter in
  // this sentence.  For simplicity's sake, we just assume the worst, that every word is a name, and
  // make sure there is that much space.  BUT if there were an enormous tacit sentence, that would be
  // very inefficient.  So, if the sentence is too long, we go through and count the number of names,
  // rather than using a poor upper bound.
  {UI4 maxnvrlen;
   if (likely(nwds < 128))maxnvrlen = (UI4)nwds;   // if short enough, assume they're all names
   else {
    maxnvrlen = 0;
    DQ(nwds, maxnvrlen+=(AT(queue[i])>>NAMEX)&1;)
   }
   // extend the nvr stack, doubling its size each time, till it can hold our names.  Don't let it get too big.  This code duplicated in 4!:55
   if(unlikely((I)(jt->parserstackframe.nvrtop+maxnvrlen) > AN(jt->nvra))){NOUNROLL do{RZ(extnvr());}while((I)(jt->parserstackframe.nvrtop+maxnvrlen) > AN(jt->nvra));}
  }

  queue+=nwds-1;  // Advance queueptr to last token.  It always points to the next value to fetch.

  // If words -1 & -2 exist, we can pull 4 words initially unless word 2 is ASGN or word 1 is EDGE or word 0 is ADV.  Unfortunately the ADV may come from a name so we also have to check in that branch
  // It has long latency so we start early.  The actual computation is about 3 cycles, much faster than a table search+misbranch
  I nopull4=0; if(likely(nwds>2))nopull4=(AT(queue[-1])|AT(queue[-2]))&ASGN+LPAR;
 
  // save $: stack.  The recursion point for $: is set on every verb execution here, and there's no need to restore it until the parse completes
  A savfs=jt->sf;  // push $: stack
#define nexty *queue  // we can't keep nexty and at both
  I nextat=AT(*queue);  // unroll fetch of at

  // allocate the stack.  No need to initialize it, except for the marks at the end, because we
  // never look at a stack location until we have moved from the queue to that position.
  // Each word gets two stack locations: first is the word itself, second the original word number+1
  // to use if there is an error on the word
  // If there is a stack inherited from the previous parse, and it is big enough to hold our queue, just use that.
  // The stack grows down
  if(unlikely((uintptr_t)jt->parserstackframe.parserstkend1-(uintptr_t)jt->parserstackframe.parserstkbgn < (nwds+BACKMARKS+FRONTMARKS)*sizeof(PSTK))){A y;
   ASSERT(nwds<65000,EVLIMIT);  // To keep the stack frame small, we limit the number of words of a sentence
   I allo = MAX((nwds+BACKMARKS+FRONTMARKS)*sizeof(PSTK),PARSERSTKALLO); // number of bytes to allocate.  Allow 4 marks: 1 at beginning, 3 at end
   GATV0(y,B01,allo,1);
   jt->parserstackframe.parserstkbgn=(PSTK*)AV(y);   // save start of data area
   // We are taking advantage of the fact the NORMAH is 7, and thus a rank-1 array is aligned on a boundary of its size
   jt->parserstackframe.parserstkend1=(PSTK*)((uintptr_t)jt->parserstackframe.parserstkbgn+allo);  // point to the end+1 of the allocation
   // We could worry about hysteresis to avoid reallocation of every call
  }
  ++jt->parsercalls;  // now we are committed to full parse.  Push stacks.
  stack=jt->parserstackframe.parserstkend1-BACKMARKS;   // start at the end, with 3 marks
  jt->parserstackframe.nvrotop=jt->parserstackframe.nvrtop;  // we have to keep the next-to-top nvr value visible for a subroutine.  It remains as we advance nvrtop.  Save in a local too for comp ease

  // We don't actually put a mark in the queue at the beginning.  When m goes down to 0, we infer a mark.

  // Set number of extra words to pull from the queue.  We always need 2 words after the first before a match is possible.  If neither of the last words is EDGE, we can take 3
  UI pt0ecam = nwds+((nopull4?0b010LL:0b100LL)<<CONJX);
  // mash into 1 register:  bit 32-63 stack0pt, bit 29-31 (from CONJX) es delayline pull 3/2/1 after current word, 
  //  (exec) 23-24,26 VJTFLGOK1+VJTFLGOK2+VASGSAFE from verb flags 27 PTNOTLPARX set if stack[0] is not (  17 set if next stack word is NOT MARK 
  //  25 ASGNLOCAL (aka SYMB) from AT(stack[0]), passed from stack to exec in case we have an assignment
  //  (exec) 20-22 savearea for pmask for lines 0-2  (stack) 17,20 flags from at NAMEBYVALUE/NAMEABANDON, 21 flag to indicate global symbol table used
  //  18-19 AR flags from symtab, 16 set if virtual last token has been processed, 0-15 m (word# in sentence)
  // bit 22,23 free
#define LOCSYMFLGX (18-ARNAMEADDEDX)
#define PLINESAVEX 20  // 3 bits of pline
// above #define USEDGLOBALX 21
// above #define USEDGLOBAL (1LL<<USEDGLOBALX)
#define NOTFINALEXECX 17
#define NOTFINALEXEC (1LL<<NOTFINALEXECX)
#define NAMEFLAGSX 20
  pt0ecam += (AR(jt->locsyms)&(ARLCLONED|ARNAMEADDED))<<LOCSYMFLGX;  // insert clone/added flags into portmanteau vbl
  // debugging if(jt->parsercalls==0xdd)
  // debugging  jt->parsercalls=0xdd;
  // DO NOT RETURN from inside the parser loop.  Stacks must be processed.

  // We have the initial stack pointer.  Grow the stack down from there
#if SY_64
#define SETSTACK0PT(v) pt0ecam=(UI4)pt0ecam, pt0ecam|=(I)(v)<<32;
#define GETSTACK0PT (pt0ecam>>32)
#define STACK0PTISCAVN (pt0ecam&(1LL<<(32+PTISCAVNX)))
#else
  UI4 stack0pt;
#define SETSTACK0PT(v) stack0pt=(v);
#define GETSTACK0PT stack0pt
#define STACK0PTISCAVN PTISCAVN(stack0pt)
#endif
  // STACK0PT contains only enough info to decode from position 0.  Upper bytes are used as flags for position 0, and bits 16-23 are used in name resolution to hold the valtype of the name
  SETSTACK0PT(PTMARK);  // will hold the EDGE+AVN value, which doesn't change much and is stored late
  stack[0].pt = stack[1].pt = stack[2].pt = PTMARK;  // install initial ending marks.  word numbers and value pointers are unused
  while(1){  // till no more matches possible...

    // no executable fragment, pull from the queue.  If we pull ')', there is no way we can execute
    // anything till 2 more words have been pulled, so we pull them here to avoid parse overhead.
    // Likewise, if we pull a CONJ, we can safely pull 1 more here.  And first time through, we should
    // pull 2 words following the first one.
    // es holds the number of extra pulls required.

    // pt0ecam is settling from pt0 but it will be ready soon
   
   do{UI tmpes;I at;A y;
     y=*(volatile A*)queue;   // fetch as early as possible
      // to make the compiler keep queue in regs, you have to convince it that the path back to this loop is common enough
      // to give it priority.  likelys at the end of the line 0-2 code are key

    if(likely((US)pt0ecam!=0)){     // if there is another valid token...
     // Move in the new word and check its type.  If it is a name that is not being assigned, resolve its
     // value.  m has the index of the word we just moved
     at=nextat;  // get type of next word.  loop was unrolled once.  We can't keep nexty and nextat in regs so we just unroll one - pity; we have to wait for AT
     // pull one value from the queue

     // We have the type of the next word, and its value is on the way.  If it is an unassigned name, we have to resolve it and perhaps use the new name/type
     if(at&PTNAMEIFASGN(~GETSTACK0PT)&NAME){L *s;  // Replace a name with its value, unless to left of ASGN.  This test is 'name and not assignment' uses the fact that NAME flag is == the flag for assignment
      // Now we have to wait for y and ->sb.sb.symx.  Transfer flags out of y into pt0ecam so that at is not needed over subroutine calls
      pt0ecam&=~(USEDGLOBAL+((NAMEBYVALUE+NAMEABANDON)>>(NAMEBYVALUEX-NAMEFLAGSX)));
      SETSTACK0PT(GETSTACK0PT&~(VALTYPEMASK>>(ADVX-PTTYPEFLAGX)))  // clear where we are going to store valtype
      pt0ecam|=(at&(NAMEBYVALUE+NAMEABANDON))>>(NAMEBYVALUEX-NAMEFLAGSX);
      // Name, not being assigned
      // Resolve the name.  If the name is x. m. u. etc, always resolve the name to its current value;
      // otherwise resolve nouns to values, and others to 'name~' references
      // The important performance case is local names with symbol numbers.  Pull that out & do it without the call overhead
      // Registers are very tight here.  y must necessarily survive over a subroutine call, but NO OTHER VARIABLES do.  If we have anything to
      // pass over a subroutine call, we have to store it pt0ecam or some other saved name
      I4 symx, buck;
#if SY_64
      I sb=NAVV(y)->sb.symxbucket; symx=sb; buck=sb>>32;  // fetch 2 values together if possible.  y is not ready until now
#else
      symx=NAV(y)->sb.sb.symx; buck=NAV(y)->sb.sb.bucket;
#endif
      I bx=NAVV(y)->bucketx;  // get an early fetch in case we don't have a symbol but we do have buckets - globals, mainly
      L *sympv=JT(jt,sympv);  // fetch the base of the symbol table.  This can't change between executions but there's no benefit in fetching earlier
      if((((I)symx-1)|SGNIF(pt0ecam,LOCSYMFLGX+ARLCLONEDX))>=0){  // if we are using primary table and there is a symbol stored there...
       s=sympv+(I)symx;  // get address of symbol in primary table
       if(unlikely(s->valtype==0))goto rdglob;  // if value has not been assigned, ignore it.  Could just treat as undef
       SETSTACK0PT(GETSTACK0PT|(s->valtype<<PTTYPEFLAGX))  // save the type
      }else if(likely(buck!=0)){  // buckets but no symbol - must be global or recursive symtab - but not synthetic name
       if((bx|SGNIF(pt0ecam,ARNAMEADDEDX+LOCSYMFLGX))>=0)goto rdglob;  // if positive bucketx and no name has been added, skip the search - the usual case if not recursive symtab
       if((s=probelocalbuckets(sympv,y,LXAV0(jt->locsyms)[buck],bx))==0)goto rdglob;  // see if there is a local symbol, using the buckets
       if(unlikely(s->valtype==0))goto rdglob;  // if value has not been assigned, ignore it.
       SETSTACK0PT(GETSTACK0PT|(s->valtype<<PTTYPEFLAGX))  // save the type
      }else{
       // No bucket info.  Usually this is a locative/global, but it could be an explicit modifier, console level, or ".
       // If the name has a cached reference, use it
       if(NAV(y)->cachedref!=0){  // if the user doesn't care enough to turn on caching, performance must not be that important
        // Note: this cannot be a NAMEABANDON, because such a name is never stacked where it can have the cachedref filled in
        A cachead=NAV(y)->cachedref; // use the cached address
        if(unlikely(NAV(y)->flag&NMCACHEDSYM)){cachead=(A)((sympv[(I)cachead]).val); if(unlikely(!cachead)){jsignal(EVVALUE);FP}}  // if it's a symbol index, fetch that.  value error only if cached symbol deleted
        y=cachead; at=AT(y); goto endname; // take its type, proceed
       }
rdglob: ;  // here when we tried the buckets and failed
       jt->parserstackframe.parsercurrtok = (US)pt0ecam;  // syrd can fail, so we have to set the error-word number (before it is decremented) before calling
       s=syrdnobuckets(y);  // do full symbol lookup, knowing that we have checked for buckets already
        // In case the name is assigned during this sentence (including subroutines), remember the data block that the name created
        // NOTE: the nvr stack may have been relocated by action routines, so we must refer to the global value of the base pointer
        // Stack a named value only once.  This is needed only for names whose VALUE is put onto the stack (i. e. a noun); if we stack a REFERENCE
        // (via namerefacv), no special protection is needed.  And, it is not needed for local names, because they are inaccessible to deletion in called
        // functions (that is, the user should not use u. to delete a local name).  If a local name is deleted, we always defer the deletion till the end of the sentence, easier than checking
        // When NVR is set, AM is used to hold the count of NVR stacking, so we can't have NVR and NJA both set.  User manages NJAs separately anyway
       if(likely(s!=0)){
        if(likely(s->valtype!=0)){  // if value has not been assigned, ignore it.
         SETSTACK0PT(GETSTACK0PT|(s->valtype<<PTTYPEFLAGX))  // save the type
         if(GETSTACK0PT&(CONW>>(ADVX-PTTYPEFLAGX))){A sv=s->val;   // this is testing for saved NOUN type
          // Normally local variables never get close to here because they have bucket info.  But if they are computed assignments,
          // or inside eval strings, they may come through this path.  If one of them is y, it might be virtual.  Thus, we must make sure we don't
          // damage AM in that case.  We don't need NVR then, because locals never need NVR.  Similarly, an LABANDONED name does not have NVR semantics, so leave it alone
          if(likely(!(AFLAG(sv)&AFNJA+AFVIRTUAL)))if(likely((AM(sv)&AMNV)!=0)){
           // NOTE that if the name was deleted in another task s->val will be invalid and we will crash
           AMNVRINCR(sv)  // add 1 to the NVR count, now that we are stacking
           AAV1(jt->nvra)[jt->parserstackframe.nvrtop++] = sv;   // record the place where the value was protected, so we can free it when this sentence completes
          }  // if NJA/virtual, leave NVR alone
         }
        }else goto undefname;  // no val
       }else goto undefname;  // no sym
       pt0ecam|=USEDGLOBAL;  // indicate that the symbol was not in the local table
      }

      // end of looking at local/global symbol tables
      // s has the symbol for the name.  pt0ecam&USEDGLOBAL is set if the name was found in a global table.  The type from valtype is in spare bits of GETSTACK0PT
      // since we have called subroutines, we don't use sympv, refetching it instead
      if(likely(1)){   // if symbol was defined...
         // Following the original parser, we assume this is an error that has been reported earlier.  No ASSERT here, since we must pop nvr stack
       // The name is defined.  If it's a noun, use its value (the common & fast case)
       // Or, for special names (x. u. etc) that are always stacked by value, keep the value
       // If a modifier has no names in its value, we will stack it by value.  The Dictionary says all modifiers are stacked by value, but
       // that will make for tough debugging.  We really want to minimize overhead for each/every/inv.
       // But: if the name is any kind of locative, we have to have a full nameref so unquote can switch locales: can't use the value then
       // Otherwise (normal adv/verb/conj name), replace with a 'name~' reference
#if SY_64  // unfortunately the compiler can't figure out that these tests are to the same register
       if(pt0ecam&((NAMEBYVALUE>>(NAMEBYVALUEX-NAMEFLAGSX))|((CONW>>(ADVX-PTTYPEFLAGX))<<32))){   // use value if noun or special name, or name::
#else
       if((pt0ecam&(NAMEBYVALUE>>(NAMEBYVALUEX-NAMEFLAGSX)))|(GETSTACK0PT&(CONW>>(ADVX-PTTYPEFLAGX)))){   // use value if noun or special name, or name::  This would be faster if the compiler knew to use a single test inst
#endif
        if(unlikely((pt0ecam&(NAMEABANDON>>(NAMEBYVALUEX-NAMEFLAGSX))))){y=namecoco(jtinplace, y, pt0ecam, s);}  // if name::, go delete the name, leaving the value to be deleted later
        else y=s->val;
        at=VALTYPETOATYPE((GETSTACK0PT>>PTTYPEFLAGX)&(VALTYPEMASK>>ADVX));  // convert saved s->valtype to AT type (calling all nouns boolean)
       }else if(unlikely(GETSTACK0PT&(NAMELESSMOD>>(ADVX-PTTYPEFLAGX)) && !(NAV(y)->flag&NMLOC+NMILOC+NMIMPLOC+NMDOT))){
        // nameless modifier, and not a locative.  Don't create a reference; maybe cache the value
        if(NAV(y)->flag&NMCACHED){
         // cachable and not a locative (and not a noun).  store the value in the name, and flag that it's a symbol index, flag the value as cached in case it gets deleted
         NAV(y)->cachedref=(A)(s-JT(jt,sympv)); NAV(y)->flag|=NMCACHEDSYM; s->flag|=LCACHED; NAV(y)->sb.sb.bucket=0;  // clear bucket info so we will skip that search - this name is forever cached
        }
        y=s->val; at=VALTYPETOATYPE((GETSTACK0PT>>PTTYPEFLAGX)&(VALTYPEMASK>>ADVX));
       }else{  // not a noun/nonlocative-nameless-modifier.  Make a reference
        y = namerefacv(y, s);   // Replace other acv with reference
        EPZ(y)
        at=AT(y);  // refresh the type with the type of the resolved name
       }
      } else {
undefname:
       // undefined name.  If special x. u. etc, that's fatal; otherwise create a dummy ref to [: (to have a verb)
       if(pt0ecam&(NAMEBYVALUE>>(NAMEBYVALUEX-NAMEFLAGSX))){jsignal(EVVALUE);FP}  // Report error (Musn't ASSERT: need to pop all stacks) and quit
       y = namerefacv(y, s);    // this will create a ref to undefined name as verb [:
       EPZ(y)
         // if syrd gave an error, namerefacv may return 0.  This will have previously signaled an error
       at=AT(y);  // refresh the type with the type of the resolved name
      }
endname: ;
     }
     // names have been resolved
     // y has the resolved value, which is never a NAME unless there is an assignment immediately following.
     // Look to see if it is ) or a conjunction,
     // which allow 2 or 1 more pulls from the queue without checking for an executable fragment.
     // Also, dyad executions sometimes allow two pulls if the first one is AVN.
     stack[-1].t = (US)pt0ecam;  // install the original token number for the word
     --pt0ecam;  //  decrement token# for the word we just processed
     queue+=REPSGN(-(I)(US)pt0ecam); nextat=AT(*(volatile A*)queue);    // fetch the next AT from unroll - the word itself follows shortly.  we can fetch queue[-1], but not AT(queue[-1)
     --stack;  // back up to new stack frame, where we will store the new word
     I it; PTFROMTYPEASGN(it,at);   // convert type to internal code
     pt0ecam&=(I)(UI4)~ASGNLOCAL;  // clear the local-assignment flag, and all of the stackpt0 field if any.  This is to save 2 fetches in executing lines 0-2 for =:
     pt0ecam|=at&((3LL<<CONJX)|ASGNLOCAL);  // calculate pull count es (2 if RPAR, 1 if CONJ, 0 otherwise); OR it in: 000= no more, other 0xx=1 more, 1xx=2 more.  Also bring in LOCAL flag for when we execute an assignment
// obsolete      pt0ecam&=~(VERB<<1)|-(at&ADV+VERB+NOUN);  // if the action routine left VERB+1 set, it means we should stack another word of we stack an AVN
     tmpes=pt0ecam;  // pt0ecam is going to be settling because of stack0pt.  To ratify the branch faster we save the relevant part

     // Put new word onto the stack along with a code indicating part of speech and the token number of the word
     SETSTACK0PT(it) stack[0].pt=it;   // stack the internal type too.  We split the ASGN types into with/without name to speed up IPSETZOMB.  Save pt in a register to avoid store forwarding.  Only parts have to be valid; we use the rest as flags
         // and to reduce required initialization of marks.  Here we take advantage of the fact the CONW is set as a flag ONLY in ASGN type
     stack[0].a = y;   // finish setting the stack entry, with the new word
    }else{  // No more tokens.  If m was 0, we are at the (virtual) mark; otherwise we are finished
     --stack;  // back up to new stack frame, where we will store the new word
     if(!(pt0ecam&0x10000)){pt0ecam|=0x10000; SETSTACK0PT(PTMARK) break;}  // first time m=0.  realize the virtual mark and use it.  a and pt will not be needed.  e and ca flags immaterial
     EP       // second time.  there's nothing more to pull, parse is over.  This is the normal end-of-parse (except for after assignment)
     // never fall through here
    }
    // *** here is where we exit stacking to do execution ***
    if(!(tmpes&(0b111LL<<CONJX)))break;  // exit stack phase when no more to do, leaving es=0
    pt0ecam=(pt0ecam&~(0b111LL<<CONJX))|((tmpes>>1)&(0b011LL<<CONJX)&~((at&ADV)<<(CONJX+1-ADVX)));  // bits 31-29: 1xx->010 01x->001 others->000.  But if ADV, change request for 3 to request for 2.  Still worth it
      // because we will be waiting for pt0 and the extra work can be done while it is settling
   }while(1);  // Repeat if more pulls required.  We also exit with stack==0 if there is an error
   // words have been pulled from queue.

  // Now execute fragments as long as there is one to execute
   while(1) {
    // This is where we execute the action routine.  We give it the stack frame; it is responsible
    // for finding its arguments on the stack, storing the result (if no error) over the last
    // stack entry, then closing up any gap between the front-of-stack and the executed fragment,
    // and finally returning the new front-of-stack pointer

    // First, create the bitmask of parser lines that are eligible to execute
    I pmask1=(I)((C*)&stack[1].pt)[1] & (I)((C*)&stack[2].pt)[2];  // stkpos 0-2 are enough to detect a match on line 0
    I pmask=(I)((C*)&stack[3].pt)[3];   // this detects  matches on lines 0-7.  LPAR is unchecked
// obsolete //    A fs1=atomic_load((_Atomic(A)*)&stack[1].a), fs=atomic_load((_Atomic(A)*)&stack[2].a);  // Read both possibilities to reduce latency
    A fs1=*(volatile A *)&stack[1].a, fs=*(volatile A *)&stack[2].a;  // Read both possibilities to reduce latency
    pmask1&=GETSTACK0PT; pmask&=pmask1;      // combine.  At this point all regs are full and if we extend pmask1 any farther it will spill
    // We have a long chain of updates to pt0ecam; start them now.  Also, we need fs and its flags; get them as early as possible
    pt0ecam&=~(CONJ+VJTFLGOK1+VJTFLGOK2+VASGSAFE+PTNOTLPAR+NOTFINALEXEC+(7LL<<PLINESAVEX));   // clear all the flags we will use
    pt0ecam|=-(nextat&ADV+NAME+VERB+NOUN)&CONJ;  // save next-is-CAVN status in CONJ (which will become the request for a 2nd pull)
// obsolete     pmask=(pmask|PTNOTLPAR)&(GETSTACK0PT^PTNOTLPAR);  // low 8 bits are lines0-7; LPAR is at some higher noncontiguous location
// obsolete //    A fs=fsa[0].a;  // 
    PSTK *fsa=&stack[2-(pmask1&1)];  // pointer to stack slot the CAV to be executed, for lines 0-4
    pt0ecam|=(!PTISM(fsa[2]))<<NOTFINALEXECX;  // remember if there is something on the stack after thie result of this exec
    fs=pmask1&1?fs1:fs;
    if(pmask){  // If all 0, nothing is dispatchable, go push next word
// obsolete      A fs=((volatile PSTK *)stack)[2-(pmask&1)].a;  // the executed self block, valid for lines 0-4 - fetch as early as possible - stk[2] except for line 0
     // We are going to execute an action routine.  This will be an indirect branch, and it will mispredict.  To reduce the cost of the misprediction,
     // we want to pile up as many instructions as we can before the branch, preferably getting out of the way as many loads as possible so that they can finish
     // during the pipeline restart.  The perfect scenario would be that the branch restarts while the loads for the stack arguments are still loading.
     // We also have a couple of branches before the indirect branch, and we try to similarly get some computation going before them
// obsolete      I pline=CTTZ(pmask);  // Get the # of the highest-priority line   scaf delete this, just mask to 1 bit
     // Save the stackpointer in case there are calls to parse in the names we execute
     jt->parserstackframe.parserstkend1=stack;
     // Fill in the token# (in case of error) based on the line# we are running
     if(pmask&0x1F){
      I fsflag=FAVV(fs)->flag;  // fetch flags early - we always need them in lines 0-2
      pt0ecam|=fsflag&VJTFLGOK1+VJTFLGOK2+VASGSAFE;  // insert flags into portmanteau reg.  This ties up the reg while flags settle, but it's mostly used for predictions
      pmask=LOWESTBIT(pmask);   // leave only one bit
      jt->parserstackframe.parsercurrtok = fsa[0].t;   // in order 4-0: 2 2 2 2 1
      // Here for lines 0-4, which execute the entity pointed to by fs
      // We will be making a bivalent call to the action routine; it will be w,fs,fs for monads and a,w,fs for dyads (with appropriate changes for modifiers).  Fetch those arguments
      // We have fs already.  arg1 will come from position 2 3 1 1 1 depending on stack line; arg2 will come from 1 2 3 2 3
      if(pmask&7){A y;  // lines 0 1 2, verb execution
       // Verb execution (in order: V N, V V N, N V N).  We must support inplacing, including assignment in place, and support recursion
       // Most of the executed fragements are executed right here.  In two cases we can be sure that the stack does not need to be rescanned:
       // 1. pline=2, token 0 is AVN: we have just put a noun in the first position, and if that produced an executable it would have been executed earlier.
       // 2. pline=0 or 2, token 0 not LPAR (might be EDGE): similarly can't execute with noun now in slot 1 (if LPAR and line 0/2, the only possible exec is () )
       // Since if pline is 0 token 0 must be EDGE, this is equivalent to pline!=1 and word 0 not LPAR
       // we save a pass through the matcher in those cases.  The 8 cycles are worth saving, but more than that it makes the branch prediction tighter
       // further, if word 0 is (C)AVN, we can pull 2 tokens if the next token is AVN: we have a flag for that
// obsolete        pt0ecam|=PTISCAVN(GETSTACK0PT)<<((VERBX+1)-PTISCAVNX);  // 0x400000 if CAVN (which implies AVN here); move flag to VERBX+1    This line & the next may not be necessary (moving within same reg)
// obsolete        pt0ecam|=GETSTACK0PT&PTNOTLPAR;  // not ( - we could do this later but here pt0ecam is tied up till fsflag settles
       pt0ecam|=pmask<<PLINESAVEX;  // lose pline over the subroutine calls to try to prevent a register spill
       // If it is an inplaceable assignment to a known name that has a value, remember the name and the value
       // We handle =: N V N, =: V N, =: V V N.  In the last case both Vs must be ASGSAFE.  When we set jt->asginfo.assignsym we are warranting
       // that the next assignment will be to the name, and that the reassigned value is available for inplacing.  In the V V N case,
       // this may be over two verbs
       // Get the branch-to address.  It comes from the appropriate valence of the appropriate stack element.  Stack element is 2 except for line 0; valence is monadic for lines 0 1 4
       jt->sf=fs;  // set new recursion point for $:
       if(unlikely((UI)((fsflag>>(pmask>>2))&VJTFLGOK1)>(UI)PTISNOTASGNNAME(GETSTACK0PT)))if(likely(!(pt0ecam&NOTFINALEXEC))){L *s;   // inplaceable assignment to name; nothing in the stack to the right of what we are about to execute; well-behaved function (doesn't change locales)
        // We have many fetches to do and they will delay the execution of the code in this block.  We will rejoin the non-assignment block with a large slug of
        // instructions that have to wait.  Probably the frontend will still be emitting blocked instructions even after all the unblocked ones have been executed.  Pity.
 // obsolete         I savpt0ecam=pt0ecam;  // the flags we need to check are ready.  Save them while we set others.  This copy will not survive the subroutine calls
 // obsolete         pt0ecam&=(FAVV(stack[1].a)->flag|((~pmask)<<(VASGSAFEX-1)))|~VASGSAFE;  // if executing line 1, make sure stack[1] is also ASGSAFE
        if(fsflag&VASGSAFE&&(!(pmask&2)||FAVV(stack[1].a)->flag&VASGSAFE)){  // if executing line 1, make sure stack[1] is also ASGSAFE
 // obsolete        if(likely((AT(stack[0].a))&ASGNLOCAL)){
         if(likely(pt0ecam&ASGNLOCAL)){
          // local assignment.  First check for primary symbol.  We expect this to succeed
          if(likely((SGNIF(pt0ecam,LOCSYMFLGX+ARLCLONEDX)|((I)NAV(nexty)->sb.sb.symx-1))>=0)){  // if we are using primary table and there is a symbol stored there...
           s=JT(jt,sympv)+(I)NAV(nexty)->sb.sb.symx;  // get address of symbol in primary table.  There may be no value; that's OK
          }else{s=jtprobeislocal(jt,nexty);}
         }else s=probeisquiet(nexty);  // global assignment, get slot address
         // Don't remember the assignand if it may change during execution, i. e. if the verb is unsafe.  For line 1 we have to look at BOTH verbs that come after the assignment
// obsolete         s=pt0ecam&VASGSAFE?s:0;  // pline is 0-2; if not 1, ignore 2nd stkpos  scaf move this earlier?  they run eventually
         // It is OK to remember the address of the symbol being assigned, because anything that might conceivably create a new symbol (and thus trigger
         // a relocation of the symbol table) is marked as not ASGSAFE
         jt->asginfo.assignsym=s;  // remember the symbol being assigned.  It may have no value yet, but that's OK - save the lookup
         // to save time in the verbs (which execute more often than this assignment-parse), see if the assignment target is suitable for inplacing.  Set zombieval to point to the value if so
         // We require flags indicate not read-only, and usecount==1 (or 2 if NJA block)
         s=s?s:SYMVAL0; A zval=s->val; zval=zval?zval:AFLAG0; zval=AC(zval)==(((AFLAG(zval)&AFRO)-1)&(((AFLAG(zval)&AFNJA)>>1)+1))?zval:0; jt->asginfo.zombieval=zval;  // needs AFRO=1, AFNJA=2
         // These instructions take a while to execute; they will probably be running when the pipeline breaks
         pmask=(pt0ecam>>PLINESAVEX)&7;  // restore after call
        }
       }
       // There is no need to set the token number in the result, since it must be a noun and will never be executed
       // Close up the stack.  For lines 0&2 we don't need two writes, so they are duplicates
       AF actionfn=FAVV(fs)->valencefns[pmask>>2];  // the routine we will execute.  We have to wait till after the register pressure or the routine address will be written to memory
       A arg2=stack[(pmask>>1)+1].a;   // 2nd arg, fs or right dyad  1 2 3 (2 3)
       A arg1=stack[(pmask&3)+1].a;   // 1st arg, monad or left dyad  2 3 1 (1 1)     0 1 2 -> 1 2 3 + 1 1 2 -> 2 3 5 -> 2 3 1
       stack[((pmask&3)>>1)+1]=stack[pmask>>1];    // overwrite the verb with the previous cell - 0->1  1->2  2->1(NOP)
       stack[pmask>>1]=stack[0];  // close up the stack  0->0(NOP)  0->1   0->2
       stack+=(pmask>>2)+1;   // finish relocating stack   1 1 2 (1 2)
       // When the args return from the verb, we will check to see if any were inplaceable and unused.  But there is a problem:
       // the arg may be freed by the verb (if it is inplaceable and gets replaced by a virtual reference).  In this case we can't
       // rely on *arg[12].  But if the value is inplaceable, the one thing we CAN count on is that it has a tpop slot.  So we will save
       // the address of the tpop slot IF the arg is inplaceable now.  Then after execution we will pick up again, knowing to quit if the tpop slot
       // has been zapped.  We keep pointers for a/w rather than 1/2 for branch-prediction purposes
       // This calculation should run to completion while the expected misprediction is being processed
       A *tpopw=AZAPLOC(arg2); tpopw=(A*)((I)tpopw&REPSGN(AC(arg2)&((AFLAG(arg2)&(AFVIRTUAL|AFUNINCORPABLE))-1))); tpopw=tpopw?tpopw:ZAPLOC0;  // point to pointer to arg2 (if it is inplace) - only if dyad
       A *tpopa=AZAPLOC(arg1); tpopa=(A*)((I)tpopa&REPSGN(AC(arg1)&((AFLAG(arg1)&(AFVIRTUAL|AFUNINCORPABLE))-1))); tpopa=tpopa?tpopa:ZAPLOC0; // obsolete  tpopw=(pmask&4)?tpopw:ZAPLOC0; // monad: w fs  dyad: a w   if monad, change to w w  
        // tpopw may point to fs, but who cares?  If it's zappable, best to zap it now
       y=(*actionfn)((J)((I)jt+(REPSGN(SGNIF(pt0ecam,VJTFLGOK1X+(pmask>>2)))&((pmask>>1)|1))),arg1,arg2,fs);   // set bit 0, and bit 1 if dyadic, if inplacing allowed by the verb
         // could use jt->sf to free fs earlier; we are about to break the pipeline.  But when we don't break we lose time waiting for jt->fs to settle
       // expect pipeline break
RECURSIVERESULTSCHECK
#if MEMAUDIT&0x10
       auditmemchains();  // trap here while we still point to the action routine
#endif
       EPZ(y);  // fail parse if error
#if AUDITEXECRESULTS
       auditblock(jt,y,1,1);
#endif
#if MEMAUDIT&0x2
       if(AC(y)==0 || (AC(y)<0 && AC(y)!=ACINPLACE+ACUC1))SEGFAULT; 
       audittstack(jt);
#endif
       stack[1+((pt0ecam>>(PLINESAVEX+1))&1)].a=y;  // save result 2 3 2; parsetype is unchanged, token# is immaterial
       // free up inputs that are no longer used.  These will be inputs that are still inplaceable and were not themselves returned by the execution.
       // We free them right here, and zap their tpop entry to avoid an extra free later.
       // We free using fanapop, which recurs only on recursive blocks, because that's what the tpop we are replacing does
       // We can free all DIRECT blocks, and PRISTINE also.  We mustn't free non-PRISTINE boxes because the contents are at large
       // and might be freed while in use elsewhere.
       // We mustn't free VIRTUAL blocks because they have to be zapped differently.  When we work that out, we will free them here too
       // NOTE that AZAPLOC may be invalid now, if the block was raised and then lowered for a period.  But if the arg is now abandoned,
       // and it was abandoned on input, and it wasn't returned, it must be safe to zap it using the zaploc BEFORE the call
       {
// obsolete        tpopa=y==*tpopa?ZAPLOC0:tpopa;  // this allows us to lose y over the subroutine call to jtra, at the expense of unsettling tpopa scaf 
       if(arg1=*tpopw){  // if the arg has a place on the stack, look at it to see if the block is still around
        I c=(UI)AC(arg1)>>(arg1==y);  // get inplaceability; set off if the arg is the result
        if((c&(-(AT(arg1)&DIRECT)|SGNIF(AFLAG(arg1),AFPRISTINEX)))<0){   // inplaceable and not return value.  Sparse blocks are never inplaceable
         *tpopw=0; tpopw=tpopa; fanapop(arg1,AFLAG(arg1));  // zap the top block; if recursive, fa the contents.  We free tpopa before subroutine
        }else tpopw=tpopa;
       }else tpopw=tpopa;
       if(arg1=*tpopw){  // if arg1==arg2 this will never load a value requiring action
        I c=(UI)AC(arg1)>>(arg1==y);   // can remove y here, see above
        if((c&(-(AT(arg1)&DIRECT)|SGNIF(AFLAG(arg1),AFPRISTINEX)))<0){  // inplaceable, not return value, not same as arg1, dyad.  Safe to check AC even if freed as arg1
         *tpopw=0; fanapop(arg1,AFLAG(arg1));
        }
       }
#if MEMAUDIT&0x2
       audittstack(jt);
#endif
       }
       // Handle early exits: AVN on line (0)12 or (not LPAR on line 02 and finalexec).
       // If line 02 and the current word is (C)AVN and the next is also, stack 2
       // the likelys on the next 2 lines are required to get the compiler to avoid spilling queue or nextat
       if(likely((GETSTACK0PT&PTNOTLPAR)!=0)){
// obsolete        if(((pt0ecam&PTNOTLPAR+(1LL<<PLINESAVEX))==PTNOTLPAR)){
        if(likely(STACK0PTISCAVN>=(pt0ecam&NOTFINALEXEC+(1LL<<(PLINESAVEX+1))))){   // test is AVN or (NOTFINAL and pmask[1] both 0)
         // not ( and (AVN or !line1 & finalexec)): OK to skip the executable check
         pt0ecam&=(((GETSTACK0PT<<(CONJX-PTISCAVNX))&~(pt0ecam<<(CONJX-(PLINESAVEX+1))))|~CONJ);  // Optionally stack one more.  CONJ is now set to (next is CAVN).  Stack 2 if also (curr is CAVN) and (line 02)
         break;  // Go stack.
        }
// obsolete        pt0ecam&=~(VERB<<1);  // if no bypass, clear the 'Pull another' flag
       }else{
        // if LPAR, the usual next thing is ( CAVN ) and we will catch that here, to avoid going through fragment search (questionable)
        if(PTISRPAR0(stack[2].pt)==0){stack[2]=stack[1]; stack[2].t=stack[0].t; SETSTACK0PT(PTNOUN); stack+=2;}  // ( CAVN ).  Handle it
       }
       // If EDGE on line 1, we must rescan for EDGE V V N
       pt0ecam&=~CONJ;  // if we are not going to stack right away, clear the request for 2d stack
      }else{
       // Lines 3-4, adv/conj execution.  We must get the parsing type of the result, but we don't need to worry about inplacing or recursion
       pmask>>=3; // 1 for adj, 2 for conj
       AF actionfn=FAVV(fs)->valencefns[pmask-1];  // the routine we will execute.  It's going to take longer to read this than we can fill before the branch is mispredicted, usually
       jt->parserstackframe.parsercurrtok = stack[2].t;   // in order 9-0: 0 0 1 1 1 2 2 2 2 1->00 00 01 01 01 10 10 10 10 01->0000 0101 0110 1010 1001
       A arg1=stack[1].a;   // 1st arg, monad or left dyad
       A arg2=stack[pmask+1].a;   // 2nd arg, fs or right dyad
       UI4 restok=stack[1].t;  // save token # to use for result
       stack[pmask]=stack[0]; // close up the stack
       stack=stack+pmask;  // advance stackpointer to position before result 1 2
       A y=(*actionfn)(jt,arg1,arg2,fs);
RECURSIVERESULTSCHECK
#if MEMAUDIT&0x10
       auditmemchains();  // trap here while we still point to the action routine
#endif
       EPZ(y);  // fail parse if error
#if AUDITEXECRESULTS
       auditblock(jt,y,1,1);
#endif
#if MEMAUDIT&0x2
       if(AC(y)==0 || (AC(y)<0 && AC(y)!=ACINPLACE+ACUC1))SEGFAULT; 
       audittstack(jt);
#endif
       PTFROMTYPE(stack[1].pt,AT(y)) stack[1].t=restok; stack[1].a=y;   // save result, move token#, recalc parsetype
      }
     }else{
      // Here for lines 5-7 (fork/hook/assign), which branch to a canned routine
      // It will run its function, and return the new stackpointer to use, with the stack all filled in.  If there is an error, the returned stackpointer will be 0.
      // We avoid the indirect branch, which is very expensive
      jt->parserstackframe.parsercurrtok = stack[1].t;   // 1 for hook/fork/assign; N/C for paren which can't fail
      if(pmask&0b10000000){  // assign - can't be fork/hook
// obsolete        if(pmask==0b10000000){   // assign
        // no need to update stack0pt because we always stack a new word after this
       stack=jtis(jt,stack); // perform assignment
       EPZ(stack)  // fail if error
       // it impossible for the stack to be executable.  If there are no more words, the sentence is finished.
       if(likely((US)pt0ecam==0)){stack-=2; EP;}  // In the normal sentence name =: ..., we are done after the assignment
       // here we are dealing with the uncommon case of non-final assignment.  If the next word is not LPAR, we can fetch another word after.
       // if the 2d-next word exists, and it is (C)AVN, and the current top-of-stack is not ADV, and the next word is not ASGN, we can pull a third word.  (Only ADV can become executable in stack[2]
       // if it was not executable next to ASGN).  We go to the trouble (1) because the case is the usual one and we are saving a little time; (2) by eliminating
       // failing calls to the parser we strengthen its branch prediction
       // At this point pt0ecam&CONJ is set to (nextat is CAVN).  For comp ease we use this instead of checking for LPAR and ASGN.  This means that for a =: b =: c we will miss after assigning b.
// obsolete         pt0ecam|=(~nextat&LPAR)<<(CONJX-LPARX);   // request to pull a second token if not (
// obsolete         if(likely((pt0ecam&(1LL-(I)(US)pt0ecam)&CONJ)!=0)){pt0ecam|=-(AT(queue[-1])&ADV+VERB+NOUN+NAME)&~(AT(stack[0].a)<<(CONJX+1-ADVX))&~(nextat<<(CONJX+1-ASGNX))&(CONJ<<1);}  // we start with CONJ set to 'next is not LPAR'
       if(likely((pt0ecam&(1LL-(I)(US)pt0ecam)&CONJ)!=0)){pt0ecam|=-(AT(queue[-1])&ADV+VERB+NOUN+NAME)&~(AT(stack[0].a)<<(CONJX+1-ADVX))&(CONJ<<1);}  // we start with CONJ set to 'next is CAVN'
       break;  // go pull the next word
// obsolete        }
      }else{
       if(pmask&0b100000)stack=jtpfork(jt,stack); else stack=jtphook(jt,stack);  // bottom of stack unchanged
       EPZ(stack)  // fail if error
      }
#if MEMAUDIT&0x10
      auditmemchains();  // trap here while we still have the parseline
#endif
#if AUDITEXECRESULTS
      if(pline<=6)auditblock(jt,stack[1].a,1,1);  // () and asgn have already been audited
#endif
#if MEMAUDIT&0x2
      if(m>=0 && (AC(stack[0].a)==0 || (AC(stack[0].a)<0 && AC(stack[0].a)!=ACINPLACE+ACUC1)))SEGFAULT; 
      audittstack(jt);
#endif
     }  // end of classifying fragment
    // the compiler doesn't handle the combination of likely and break.  If we don't put something here, the fail-parse branch will go backwards
    // and will predict that way, which is wrong.
    }else{
     // LPAR misses the main parse table, which is just as well because it would miss later branches anyway.  We pick it up here so as not to add
     // a couple of cycles to the main parse test
     if(!(GETSTACK0PT&PTNOTLPAR)){  // ( with no other line.  Better be ( CAVN )
      if(likely(PTISCAVN(~stack[1].pt)==PTISRPAR0(stack[2].pt))){  // must be [1]=CAVN and [2]=RPAR.  To be equal, !CAVN and RPAR-if-0 must both be 0 
       SETSTACK0PT(stack[1].pt); stack[2]=stack[1]; stack[2].t=stack[0].t;  //  Install result over ).  Use value/type from expr, token # from (   Bottom of stack was modified, so refresh the type for it
       stack+=2;  // advance stack pointer to result
      }else{jsignal(EVSYNTAX); FP}  // error if contents of ( not valid
      // we fall through to rescan after ( )
     }else{pt0ecam&=~CONJ;  break;}   // parse failed, return to stack next word.  Must clear 'stack 2' flag
    }  // end 'there was a fragment'
   } // end of loop executing fragments
   
  }  // break with stack==0 on error; main exit is when queue is empty (m<0)
 exitparse:
   // Prepare the result

  if(likely(stack!=0)){  // if no error yet...
   // before we exited, we backed the stack to before the initial mark entry.  At this point stack[0] is invalid,
   // stack[1] is the initial mark, stack[2] is the result, and stack[3] had better be the first ending mark
   z=stack[2].a;   // stack[0..1] are the mark; this is the sentence result, if there is no error
   if(unlikely(!(PTOKEND(stack[2],stack[3])))){jt->parserstackframe.parsercurrtok = 0; jsignal(EVSYNTAX); z=0;}  // OK if 0 or 1 words left (0 should not occur)
  }else{
failparse:  // If there was an error during execution or name-stacking, exit with failure.  Error has already been signaled.  Remove zombiesym
   CLEARZOMBIE z=0; stack=PSTK2NOTFINALASGN;  // set stack to something that IS NOT a final assignment
  }
#if MEMAUDIT&0x2
  audittstack(jt);
#endif

  // Now that the sentence has completed, take care of some cleanup.  Names that were reassigned after
  // their value was moved onto the stack had the decrementing of the use count deferred: we decrement
  // them now.  There may be references to these names in the result (if we are returning a verb/adv/conj),
  // so we don't free the names quite yet: we put them on the tpush stack to be freed after we know
  // we are through with the result.  If we are returning a noun, free them right away unless they happen to be the very noun we are returning
  // We apply the final free only when the NVR count goes to 0, to make sure we hold off till the last stacked reference has been seen off
  v=AAV1(jt->nvra)+jt->parserstackframe.nvrotop;  // point to our region of the nvr area
  UI zcompval = !z||AT(z)&NOUN?0:-1;  // if z is 0, or a noun, immediately free only values !=z.  Otherwise don't free anything
  DQ(jt->parserstackframe.nvrtop-jt->parserstackframe.nvrotop, A vv = *v;I am;
   // if the NVR count is 1 before we decrement, we have hit the last stacked use & we free the block.
   // if we are performing (or finally deferring) the FINAL free, the value must be a complete zombie and cannot be active anywhere; otherwise we must clear it.  We clear it always
   if(likely((AMNVRDECR(vv,am))<2*AMNVRCT)){if(am&AMFREED){AMNVRAND(vv,~AMFREED) if(((UI)z^(UI)vv)>zcompval){fanano0(vv);}else{tpushna(vv);}}}
   ++v;);   // schedule deferred frees.
    // na so that we don't audit, since audit will relook at this NVR stack

  // Still can't return till frame-stack popped
  jt->parserstackframe = oframe;
#if MEMAUDIT&0x2
  audittstack(jt);
#endif
  jt->sf=savfs;  // pop $: stack
#if MEMAUDIT&0x20
     auditmemchains();  // trap here while we still have the parseline
#endif

  // NOW it is OK to return.  Insert the final-assignment bit (sign of stack[2].t) into the return
  R (A)((I)z+SGNTO0US(stack[2].t));  // this is the return point from normal parsing

 }else{A y;  // m<2.  Happens fairly often, and full parse can be omitted
  if(likely(nwds==1)){  // exit fast if empty input.  Happens only during load, but we can't deal with it
   // Only 1 word in the queue.  No need to parse - just evaluate & return.  We do it here to avoid parsing
   // overhead, because it happens enough to notice.
   // No ASSERT - must get to the end to pop stack
   jt->parserstackframe.parsercurrtok=0;  // error token if error found
   I at=AT(y = queue[0]);  // fetch the word
   if((at&NAME)!=0) {L *s;A sv;  // pointer to value block for the name
    if(likely((((I)NAV(y)->sb.sb.symx-1)|SGNIF(AR(jt->locsyms),ARLCLONEDX))>=0)){  // if we are using primary table and there is a symbol stored there...
     s=JT(jt,sympv)+(I)NAV(y)->sb.sb.symx;  // get address of symbol in primary table
     if(likely((sv=s->val)!=0))goto got1val;  // if value has not been assigned, ignore it.  Could just treat as undef
    }
    if(likely((s=syrd(y,jt->locsyms))!=0)){     // Resolve the name.
     RZ(sv = s->val);  // symbol table entry, but no value.  Must be in an explicit definition, so there is no need to raise an error
got1val:;
     if(likely(((AT(sv)|at)&(NOUN|NAMEBYVALUE))!=0)){   // if noun or special name, use value
      if(unlikely(at&NAMEABANDON)){
       namecoco(jtinplace, y, (syrdforlocale(y)==jt->locsyms)<<USEDGLOBALX, s);  // if name::, go delete the name, leaving the value to be deleted later
      }
      y=sv;
     } else y = namerefacv(y, s);   // Replace other acv with reference.  Could fail.
    } else {
     // undefined name.
     if(at&NAMEBYVALUE){jsignal(EVVALUE); y=0;}  // Error if the unresolved name is x y etc.  Don't ASSERT since we must pop stack
     else y = namerefacv(y, s);    // this will create a ref to undefined name as verb [: .  Could set y to 0 if error
    }
   }
   if(likely(y!=0))if(unlikely(!(AT(y)&CAVN))){jsignal(EVSYNTAX); y=0;}  // if not CAVN result, error
  }else y=mark;  // empty input - return with 'mark' as the value, which means nothing to parse.  This result must not be passed into a sentence
  jt->parserstackframe = oframe;
  R y;
 }

}

