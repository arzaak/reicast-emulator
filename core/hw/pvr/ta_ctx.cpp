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

// helper for 32 byte aligned memory allocation
void* OS_aligned_malloc(size_t align, size_t size)
{
#ifdef __MINGW32__
   return __mingw_aligned_malloc(size, align);
#elif defined(_WIN32)
   return _aligned_malloc(size, align);
#else
   void *p = NULL;
   int ret = posix_memalign(&p, align, size);
   return (ret == 0) ? p : 0;
#endif
}

// helper for 32 byte aligned memory de-allocation
void OS_aligned_free(void *ptr)
{
#ifdef __MINGW32__
   __mingw_aligned_free(ptr);
#elif defined(_MSC_VER)
   _aligned_free(ptr);
#else
   free(ptr);
#endif
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

bool TryDecodeTARC(void)
{
   verify(ta_ctx != 0);
   
	if (vd_ctx == 0)
	{
		vd_ctx = ta_ctx;

		vd_ctx->rend.proc_start = vd_ctx->rend.proc_end + 32;
		vd_ctx->rend.proc_end = vd_ctx->tad.thd_data;
			
      vd_ctx->rend_inuse.Lock();
		vd_rc = vd_ctx->rend;

		//signal the vdec thread
		return true;
	}
   else
      return false;
}

void VDecEnd(void)
{
   verify(vd_ctx != 0);

	vd_ctx->rend = vd_rc;

   vd_ctx->rend_inuse.Unlock();

	vd_ctx = 0;
}

cMutex mtx_rqueue;
TA_context* rqueue;
cResetEvent frame_finished(false, true);

bool QueueRender(TA_context* ctx)
{
   verify(ctx != 0);

   if (FrameSkipping && frameskip) {
 		frameskip=1-frameskip;
		tactx_Recycle(ctx);
		fskip++;
		return false;
 	}

	if (rqueue)
   {
		tactx_Recycle(ctx);
		return false;
	}

   frame_finished.Reset();
   mtx_rqueue.Lock();
	TA_context* old = rqueue;
	rqueue=ctx;
   mtx_rqueue.Unlock();

   verify(!old);

	return true;
}

TA_context* DequeueRender(void)
{
   mtx_rqueue.Lock();
	TA_context* rv = rqueue;
   mtx_rqueue.Unlock();

	if (rv)
		FrameCount++;

	return rv;
}

bool rend_framePending(void)
{
   mtx_rqueue.Lock();
	TA_context* rv = rqueue;
   mtx_rqueue.Unlock();

	return rv != 0;
}

void FinishRender(TA_context* ctx)
{
   verify(rqueue == ctx);
   mtx_rqueue.Lock();
	rqueue = 0;
   mtx_rqueue.Unlock();

	tactx_Recycle(ctx);
   frame_finished.Set();
}

cMutex mtx_pool;

/* texture cache entry pool. */
vector<TA_context*> ctx_pool;
vector<TA_context*> ctx_list;

TA_context* tactx_Alloc(void)
{
	TA_context* rv = 0;

   mtx_pool.Lock();
	if (ctx_pool.size())
	{
		rv = ctx_pool[ctx_pool.size()-1];
		ctx_pool.pop_back();
	}
   mtx_pool.Unlock();
	
	if (!rv)
   {
      rv = new TA_context();
      rv->Alloc();
      printf("new tactx\n");
   }

   return rv;
}

void tactx_Recycle(TA_context* poped_ctx)
{
   mtx_pool.Lock();
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
   mtx_pool.Unlock();
}

TA_context* tactx_Find(u32 addr, bool allocnew)
{
   for (size_t i=0; i<ctx_list.size(); i++)
   {
      if (ctx_list[i]->Address==addr)
         return ctx_list[i];
   }

   if (allocnew)
   {
      TA_context *rv = tactx_Alloc();
      rv->Address=addr;
      ctx_list.push_back(rv);

      return rv;
   }

   return 0;
}

TA_context* tactx_Pop(u32 addr)
{
	for (size_t i=0; i<ctx_list.size(); i++)
   {
      if (ctx_list[i]->Address == addr)
      {
         TA_context *rv = ctx_list[i];

         if (ta_ctx == rv)
            SetCurrentTARC(TACTX_NONE);

         ctx_list.erase(ctx_list.begin() + i);

         return rv;
      }
   }
	return 0;
}
