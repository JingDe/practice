

/*
*	对Windows
*		#if def _WIN64
*			#define NGX_PTR_SIZE 8
*		#else
*			#define NGX_PTR_SIZE 4
*		#endif
*	对linux
*		nginx/auto目录
*		定义uintptr_t，获得sizeof？？

* 	超大内存的第一页，slab的钱3位设为NGX_SLAB_PAGE_START，
*	其余位表示紧随其后相邻的同批页面数
*	其他页的slab设为NGX_SLAB_BUSY
*/
// 用于ngx_slab_page_t的slab成员
#define NGX_SLAB_BUSY        0xffffffffffffffff

#if (NGX_PTR_SIZE==4)
#define NGX_SLAB_PAGE_START 0x80000000
#else // NGX_PTR_SIZE==8
#define NGX_SLAB_PAGE_START 0x8000000000000000
#endif



// 页类型，用于ngx_slab_page_t的prev成员，指明小块、中等、大块内存
#define NGX_SLAB_PAGE_MASK   3
#define NGX_SLAB_PAGE        0 // 超大内存
#define NGX_SLAB_BIG         1 // 大内存
#define NGX_SLAB_EXACT       2 // 中等内存
#define NGX_SLAB_SMALL       3 // 小内存



// 存放多个内存块的页面中，允许的最大内存块大小为ngx_slab_max_size
static unsigned int  ngx_slab_max_size;// 等于ngx_pagesize / 2;
static unsigned int ngx_slab_max_shift;

static unsigned int ngx_slab_exact_size;//中等内存块大小
static unsigned int ngx_slab_exact_shift;

//计算slots数组位置
#define ngx_slab_slots(pool) \
	(ngx_slab_page_t*) ((unsigned char*)(pool) + sizeof(ngx_slab_pool_t))

#define ngx_slab_junk(p, size)  memset(p, 0xA5, size)

// p向右与a对齐：(p+a-1) & ~(a-1)
#define ngx_align_ptr(p, a) \
	(unsigned char*) (((uintptr_t) (p) + ((uintptr_t) a - 1))   &    ~((uintptr_t) a - 1))

// 将pool的页面结构体指针page转换成相应的页面首地址
#define ngx_slab_page_addr(pool, page) \
	( (((page) - (pool)->pages) << ngx_pagesize_shift) \
     + (uintptr_t) (pool)->start )

#define ngx_slab_page_prev(page) \
	(ngx_slab_page_t*) ((page)->prev  &  ~NGX_SLAB_PAGE_MASK)

void ngx_slab_sizes_init()
{
	int n;
	
	ngx_pagesize=getpagesize();
	for(n=ngx_pagesize; n>>=1; ngx_pagesize_shift++){}
	
	ngx_slab_max_size=ngx_pagesize/2;
	for(n=ngx_slab_max_size; n>>=1; ngx_slab_max_shift++){}
	
	ngx_slab_exact_size=ngx_pagesize/(8*sizeof(uintptr_t));
	for(n=ngx_slab_exact_size; n>>=1; ngx_slab_exact_shift++){}
}

void ngx_slab_init(ngx_slab_pool_t *pool)
{
	unsigned char *p;
	size_t size;
	int m;
	unsigned int i, n, pages;
	ngx_slab_page_t *slots, *page;
	
	pool->min_size=(size_t) 1 << pool->min_shift;

	slots=ngx_slab_slots(pool);//计算slots数组位置
	
	p=(unsigned char*)slots;
	size=pool->end-p;//slab共享内存除pool结构体的大小
	ngx_slab_junk(p, size);// 设置p开始size字节为0xA5
	
	/*
	* 共有n种长度的内存块，slots数组和stats数组长度
	*/
	n=ngx_pagesize_shift-pool->min_shift;
	
	for(i=0; i<n; i++)
	{
		slots[i].slab=0;
		slots[i].next=&slots[i];
		slots[i].prev=0;
	}
	
	p+=n*sizeof(ngx_slab_page_t);
	
	pool->stats=(ngx_slab_stat_t*)p;
	bzero(pool->stats, n*sizeof(ngx_slab_stat_t));
	
	p+=n*sizeof(ngx_slab_stat_t);
	
	size-=n*(sizeof(ngx_slab_page_t) + sizeof(ngx_slab_stat_t));
	
	// 页面个数，pages数组长度
	pages=(unsigned int)(size / (ngx_pagesize+sizeof(ngx_slab_page_t)));
	
	// 初始化pages数组
	pool->pages=(ngx_slab_page_t*)p;
	bzero(pool->pages, pages*sizeof(ngx_slab_page_t));
	
	// page指向pages数组第一个元素
	page=pool->pages;
	
	// 初始化free链表头结点
	pool->free.slab=0;
	pool->free.next=page;
	pool->free.prev=0;
	
	// 设置第一个free链表节点为所有连续页面
	page->slab=pages;
	page->next=&pool->free;
	page->prev=(uintptr_t)&pool->free;
	
	// start指向真实页面，以页面大小对齐
	pool->start=ngx_align_ptr(p+pages*sizeof(ngx_slab_page_t), ngx_pagesize);
	
	// 由于需要地址对齐，页面个数可能减小
	m=pages-(pool->end-pool->start) / ngx_pagesize;
	if(m>0)
	{
		pages-=m;
		page->slab=pages;
	}
	
	//last指向pages数组末尾??
	pool->last=pool->pages+pages;
	pool->log_ctx=&pool->zero;
	pool->zero='\0';
}


/*
* 从slab共享内存中分配pages个页面，返回对应页面的pages数组元素
*/
static ngx_slab_page_t* ngx_slab_alloc_pages(ngx_slab_pool_t *pool, unsigned int pages)
{
	ngx_slab_page_t *page, *p;
	
	for(page=pool->free.next; page!=&pool->free; page=page->next)
	{
		
		if(page->slab>=pages)
		{
			// 将前pages个页面分配出去，剩余的连续页面数仍然作为一个链表元素放在free池中
			if(page->slab>pages)
			{
				// page[page->slab-1]表示这组连续页面中最后一个页面？？
				// page[pages] 是这组连续页面中，第一个不被使用的页，
				page[page->slab-1].prev=(uintptr_t)&page[pages];
				
				// 以下，将page[pages]插入到free链表中
				page[pages].slab=page->slab-pages;
				page[pages].next=page->next;
				page[pages].prev=page->prev;
				// 修改链表上一个元素和下一个元素的指针指向
				p=(ngx_slab_page_t*)page->prev;
				p->next=&page[pages];
				page->next->prev=(uintptr_t)&page[pages];			
			}
			else
			{
				p=(ngx_slab_page_t*)page->prev;
				p->next=page->next;
				page->next-prev=page->prev;
			}

			/*slab是uintptr_t类型。
			*	slab的前3位设为NGX_SLAB_PAGE_START，其余位表示相邻同批页面数
			*/
			page->slab=pages | NGX_SLAB_PAGE_START;
			page->next=NULL;//不在链表中
			page->prev=NGX_SLAB_PAGE;//pre定义页类型
			
			pool->pfree=pages;
			
			if(--pages==0)
				return page;
			
			// 如果分配了连续多个页面，后续页描述也许奥初始化
			for(p=page+1; pages; pages--)
			{
				p->slab=NGX_SLAB_PAGE_BUSY;// 连续页作为一个内存块一起分配出时，非第1页的slab都置为NGX_SLAB_PAGE_BUSY
				p->next=NULL;
				p->prev=NGX_SLAB_PAGE;
				p++;
			}
			return page;
		}
	}
	
	if(pool->log_nomem)
	{
		LOG_ERROR<<"ngx_slab_alloc() failed: no memory";
	}
	return NULL;
}

void* ngx_slab_alloc(ngx_slab_pool_t *pool, size_t size)
{
	void *p;
	
	MutexLock lock(&pool->mutex);
	p=ngx_slab_alloc_locked(pool, size);
	return p;
}

void* ngx_slab_alloc_locked(ngx_slab_pool_t *pool, size_t size)
{
	size_t s;
	uintptr_t p, m, mask, *bitmap;
	unsigned int i, n, slot, shift, map;
	ngx_slab_page_t *page, *prev, *slots;
	
	// 分配超大内存
	if(size>ngx_slab_max_size)
	{
		LOG_DEBUG<<"slab alloc: "<<size;
		page=ngx_slab_alloc_pages(pool, (size>>ngx_pagesize_shift)+((size%ngx_pagesize) ? 1 : 0) );
		if(page)
			p=ngx_slab_page_addr(pool, page);
		else
			p=0;
		goto done;
	}
	
	if(size>pool->min_size)
	{
		shift=1;//内存块的大小偏移量
		for(s=size-1; s>>=1; shift++){}
		slot=shift-pool->min_shift;// slot数组下标，stat数组下标
	}
	else
	{
		shift=pool->min_shift;// 最小块内存是2的min_shift大小？
		slot=0;
	}
	
	pool->stats[slot].req++;
	
	LOG_DEBUG<<"slab alloc: "<<size<<", slot: "<<slot;
	
	slots=ngx_slab_slots(pool);
	page=slots[slot].next;
	
	if(page->next !=page)
	{
		// 分配小块内存
		if(shift<ngx_slab_exact_shift)
		{
			bitmap=(uintptr_t*)ngx_slab_page_addr(pool, page);//页面首地址
			// 需要map个uintptr_t类型可以完整地表达该页面的bitmap
			map=(ngx_pagesize >> shift) / (8*sizeof(uintptr_t));
			
			for(n=0; n<map; n++)
			{
				// 通过用uintptr_t与NGX_SLAB_BUSY比较，快速排除掉全满的uintptr_t
				if(bitmap[n]!=NGX_SLAB_BUSY)
				{
					//确认当前bitmap[n]上有空闲块
					for(m=1, i=0; m; m<<=1, i++)
					{
						if(bitmap[n] & m)
							continue;
						
						bitmap[n]|=m;

						/*
						当前的bit对应着该页面的第(n*8*sizeof(uintptr_t)+i)个内存块。
						乘以shift得到该内存块在页面上的字节偏移量
						*/
						i=(n*8*sizeof(uintptr_t)+i) << shift;
						
						//p指向该空闲块的首地址
						p=(uintptr_t)bitmap+i;
						
						// 当前uintptr_T表示的内存块已占用
						if(bitmap[n]==NGX_SLAB_BUSY)
						{
							for(n=n+1; n<map; n++)
								if(bitmap[n]!=NGX_SLAB_BUSY)
									goto done;
							
							// 当前内存页已满，脱离半满页链表
							prev=ngx_slab_page_prev(page);
							prev->next=page->next;
							page->next->prev=page->prev;
							
							page->next=NULL;
							page->prev=NGX_SLAB_SMALL;
						}
						goto done;
					}
				}
			}
		}
		else if(shift==ngx_slab_exact_shift)
		{
			// TODO
		}
		else
		{}
		
		LOG_ERROR<<"ngx_slab_alloc() page is busy";
		
	}
	
	// 没有半满页
	
	page=ngx_slab_alloc_pages(pool, 1);
	
	if(page)
	{
		if(shift<ngx_slab_exact_shift)
		{
		/*
		留心n和map的计算
		*/
			// 初始化新分配页面的bitmap
			// bitmap既是页面的首地址，也是bitmap的起始
			bitmap=(uintptr_t*)ngx_slab_page_addr(pool, page);
			
			// 新页面的块大小偏移量为shift，（ngx_pagesize >> shift)表示块个数，即需要的bit个数
			// ((1<<shift)*8)表示一个块有多少比特
			// n表示需要多少个内存块才能放得下整个bitmap
			n=(ngx_pagesize >> shift) / ((1<<shift)*8);
			
			if(n==0)
				n=1;
			
			/*因为前n个内存块已经用于bitmap了，不可以再被使用，
			所以置这些内存块对应的bit位为1
			
			(n+1)/(8*sizeof(uintptr_t)  表示 用来表示放置bitmap的内存块 本身需要多少个uintptr_t来表示自身
			(n+1)%(8*sizeof(uintptr_t)) （设为X）表示 剩余用不着整个uintptr_t表示的（未被表示的）内存块的个数
			*/
			for(i=0; i<(n+1)/(8*sizeof(uintptr_t); i++);
				bitmap[i]=NGX_SLAB_BUSY;
			
			/* m = ( 1 << (    (n+1)%(8*sizeof(uintptr_t))  ) ) - 1; 
			m表示剩余未被表示的内存块，用下一个uintptr_t的高X位设为1表示的值
			*/
			m= ((uintptr_t) 1 << ((n + 1) % (8 * sizeof(uintptr_t))) ) - 1;
			bitmap[i]=m;
			
			// 设置剩下的uintptr_t的各个bit为0
			// map表示需要多少个uintptr_t才能放得下整个bitmap
			map = (ngx_pagesize >> shift) / (8 * sizeof(uintptr_t));
			
			// 设置剩余的bit位为0
			for (i = i + 1; i < map; i++) {
                bitmap[i] = 0;
            }
			
			page->slab=shift;
			page->next=&slots[slot];
			page->prev=(uintptr_t)&slots[slot] | NGX_SLAB_SMALL;
			
			slots[slot].next=page;
			
			// 统计该slot的内存块的个数
			pool->stats[slot].total+=(ngx_pagesize >> shift) -n;
			
			// TODO
		}
	}
	
	p=0;
	
	pool->stats[slot].fails++;
	
done:
	LOG_DEBUG<<"slab alloc: "<<p;
	
	return (void*)p;
}