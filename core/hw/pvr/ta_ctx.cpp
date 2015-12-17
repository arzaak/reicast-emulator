#include "ta.h"
#include "ta_ctx.h"

#include "hw/sh4/sh4_sched.h"

extern u32 fskip;
extern u32 FrameCount;

int frameskip=0;
bool FrameSkipping=false;		// global switch to enable/disable frameskip

TA_context* ta_ctx;
tad_context ta_tad;

TA_context*  vd_ctx;
rend_context vd_rc;

slock_t *mtx_rqueue;
TA_context* rqueue;
cResetEvent frame_finished(false, true);

double last_frame = 0;
u64 last_cyces = 0;

slock_t *mtx_pool;

vector<TA_context*> ctx_pool;
vector<TA_context*> ctx_list;

static TA_context* tactx_Alloc(void)
{
	TA_context* rv = 0;

   slock_lock(mtx_pool);
	if (ctx_pool.size())
	{
		rv = ctx_pool[ctx_pool.size()-1];
		ctx_pool.pop_back();
	}
   slock_unlock(mtx_pool);
	
	if (rv)
      return rv;

   rv = new TA_context();
   rv->Alloc();
   printf("new tactx\n");

	return rv;
}

static TA_context* tactx_Find(u32 addr, bool allocnew)
{
   TA_context *rv = NULL;
   for (size_t i=0; i<ctx_list.size(); i++)
   {
      if (ctx_list[i]->Address==addr)
         return ctx_list[i];
   }

   if (!allocnew)
      return 0;

   rv = tactx_Alloc();
   rv->Address=addr;
   ctx_list.push_back(rv);

   return rv;
}

void SetCurrentTARC(u32 addr)
{
	if (addr != TACTX_NONE)
	{
		if (ta_ctx)
			SetCurrentTARC(TACTX_NONE);

		verify(ta_ctx == 0);
		//set new context
		ta_ctx = tactx_Find(addr,true);

		//copy cached params
		ta_tad = ta_ctx->tad;
	}
	else
	{
		//Flush cache to context
		verify(ta_ctx != 0);
		ta_ctx->tad=ta_tad;
		
		//clear context
		ta_ctx=0;
		ta_tad.Reset(0);
	}
}

static bool TryDecodeTARC(void)
{
	verify(ta_ctx != 0);

	if (vd_ctx == 0)
	{
		vd_ctx = ta_ctx;

		vd_ctx->rend.proc_start = vd_ctx->rend.proc_end + 32;
		vd_ctx->rend.proc_end = vd_ctx->tad.thd_data;
			
      slock_lock(vd_ctx->rend_inuse);
		vd_rc = vd_ctx->rend;

		//signal the vdec thread
		return true;
	}
   return false;
}

static void VDecEnd(void)
{
	verify(vd_ctx != 0);

	vd_ctx->rend = vd_rc;

   slock_unlock(vd_ctx->rend_inuse);

	vd_ctx = 0;
}


bool QueueRender(TA_context* ctx)
{
	verify(ctx != 0);
	
	if (FrameSkipping && frameskip) {
 		frameskip=1-frameskip;
		tactx_Recycle(ctx);
		fskip++;
		return false;
 	}
 	
 	//Try to limit speed to a "sane" level
 	//Speed is also limited via audio, but audio
 	//is sometimes not accurate enough (android, vista+)
 	u32 cycle_span   = sh4_sched_now64() - last_cyces;
 	last_cyces       = sh4_sched_now64();
 	double time_span = os_GetSeconds() - last_frame;
 	last_frame       = os_GetSeconds();

 	bool too_fast = (cycle_span / time_span) > (SH4_MAIN_CLOCK * 1.2);
	
	if (rqueue && too_fast && settings.pvr.SynchronousRendering) {
		//wait for a frame if
		//  we have another one queue'd and
		//  sh4 run at > 120% on the last slice
		//  and SynchronousRendering is enabled
		frame_finished.Wait();
		verify(!rqueue);
	} 

	if (rqueue) {
		tactx_Recycle(ctx);
		fskip++;
		return false;
	}

	frame_finished.Reset();
   slock_lock(mtx_rqueue);
	TA_context* old = rqueue;
	rqueue=ctx;
   slock_unlock(mtx_rqueue);

	verify(!old);

	return true;
}

TA_context* DequeueRender(void)
{
   slock_lock(mtx_rqueue);
	TA_context* rv = rqueue;
   slock_unlock(mtx_rqueue);

	if (rv)
		FrameCount++;

	return rv;
}

bool rend_framePending(void)
{
   slock_lock(mtx_rqueue);
	TA_context* rv = rqueue;
   slock_unlock(mtx_rqueue);

	return rv != 0;
}

void FinishRender(TA_context* ctx)
{
	verify(rqueue == ctx);
   slock_lock(mtx_rqueue);
	rqueue = 0;
   slock_unlock(mtx_rqueue);

	tactx_Recycle(ctx);
	frame_finished.Set();
}



void tactx_Recycle(TA_context* poped_ctx)
{
   slock_lock(mtx_pool);
   if (ctx_pool.size()>2)
   {
      poped_ctx->Free();
      delete poped_ctx;
   }
   else
   {
      poped_ctx->Reset();
      ctx_pool.push_back(poped_ctx);
   }
   slock_unlock(mtx_pool);
}


TA_context* tactx_Pop(u32 addr)
{
	for (size_t i=0; i<ctx_list.size(); i++)
   {
      TA_context *rv = NULL;
      if (ctx_list[i]->Address != addr)
         continue;

      rv = ctx_list[i];

      if (ta_ctx == rv)
         SetCurrentTARC(TACTX_NONE);

      ctx_list.erase(ctx_list.begin() + i);

      return rv;
   }
	return 0;
}

void ta_ctx_free(void)
{
   slock_free(mtx_rqueue);
   slock_free(mtx_pool);
   mtx_rqueue = NULL;
   mtx_pool   = NULL;
}

void ta_ctx_init(void)
{
   mtx_rqueue = slock_new();
   mtx_pool   = slock_new();
}
