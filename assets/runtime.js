document.addEventListener('DOMContentLoaded',()=>{
  if(window.renderMathInElement)renderMathInElement(document.body,{delimiters:[{left:'$$',right:'$$',display:true},{left:'\\[',right:'\\]',display:true},{left:'$',right:'$',display:false},{left:'\\(',right:'\\)',display:false}],throwOnError:false});
  const referenceNotes=[...document.querySelectorAll('.reference-sidenote')];
  if(referenceNotes.length){
    const mobileNotes=new Map([...document.querySelectorAll('.mobile-notes .reference-note')]
      .map(note=>[note.dataset.noteId,note]));
    const repeatDistance=480; // Minimum vertical distance in CSS pixels between shown notes for one reference.
    const updateReferenceNotes=()=>{
      referenceNotes.forEach(note=>{
        note.hidden=false;
        const mobile=mobileNotes.get(note.dataset.noteId);
        if(mobile)mobile.hidden=false;
      });
      const lastShown=new Map();
      referenceNotes.forEach(note=>{
        const anchor=document.getElementById(note.dataset.citeAnchor);
        if(!anchor)return;
        const top=anchor.getBoundingClientRect().top+window.scrollY;
        const previous=lastShown.get(note.dataset.refNumber);
        const show=previous===undefined||top-previous>=repeatDistance;
        note.hidden=!show;
        const mobile=mobileNotes.get(note.dataset.noteId);
        if(mobile)mobile.hidden=!show;
        if(show)lastShown.set(note.dataset.refNumber,top);
      });
    };
    updateReferenceNotes();
    if(document.fonts)document.fonts.ready.then(updateReferenceNotes);
    window.addEventListener('load',updateReferenceNotes,{once:true});
    let resizePending=false;
    window.addEventListener('resize',()=>{
      if(resizePending)return;
      resizePending=true;
      requestAnimationFrame(()=>{resizePending=false;updateReferenceNotes();});
    });
  }
  const themeToggle=document.querySelector('.theme-toggle');
  if(themeToggle){
    const applyTheme=theme=>{
      document.documentElement.dataset.theme=theme;
      const dark=theme==='dark';
      themeToggle.setAttribute('aria-checked',String(dark));
    };
    applyTheme(document.documentElement.dataset.theme||'light');
    themeToggle.addEventListener('click',()=>{
      const theme=document.documentElement.dataset.theme==='dark'?'light':'dark';
      applyTheme(theme);
      try{localStorage.setItem('myne-theme',theme)}catch(_){}
    });
  }
  const dependencyGraph=document.querySelector('.dependency-graph');
  if(dependencyGraph){
    const nodeElements=[...dependencyGraph.querySelectorAll('.dependency-node')];
    const edgeElements=[...dependencyGraph.querySelectorAll('.dependency-edge')];
    const count=nodeElements.length;
    const toolbar=document.querySelector('.dependency-toolbar');
    const nodes=nodeElements.map((element,index)=>({
      element,index,width:Number(element.dataset.width),height:56,rank:0,order:0,x:0,y:0
    }));
    for(const node of nodes){
      const label=node.element.querySelector('.dependency-name');
      if(label.getComputedTextLength()>node.width-24){
        label.setAttribute('textLength',node.width-24);
        label.setAttribute('lengthAdjust','spacingAndGlyphs');
      }
    }
    const edges=edgeElements.map(element=>({
      element,from:Number(element.dataset.from),to:Number(element.dataset.to)
    }));
    const outgoing=Array.from({length:count},()=>[]),incoming=Array.from({length:count},()=>[]);
    for(const edge of edges){outgoing[edge.from].push(edge.to);incoming[edge.to].push(edge.from);}
    const edgeLayer=dependencyGraph.querySelector('.dependency-edges');
    const highlightedEdges=document.createElementNS('http://www.w3.org/2000/svg','g');
    highlightedEdges.setAttribute('class','dependency-highlighted-edges');
    highlightedEdges.setAttribute('pointer-events','none');
    dependencyGraph.append(highlightedEdges);
    const highlightedArrow=dependencyGraph.querySelector('#dep-arrow').cloneNode(true);
    highlightedArrow.id='dep-arrow-highlight';
    highlightedArrow.querySelector('path').setAttribute('class','dependency-arrow-highlight');
    dependencyGraph.querySelector('defs').append(highlightedArrow);
    const tooltip=document.querySelector('.dependency-tooltip');
    let selectedAnchor=null;
    const clearHover=()=>{
      for(const node of nodes)node.element.parentElement.classList.remove('is-active','is-dependency');
      for(const edge of edges){
        edge.element.classList.remove('is-highlighted');
        edge.element.setAttribute('marker-end','url(#dep-arrow)');
        edgeLayer.append(edge.element);
      }
      if(tooltip){tooltip.hidden=true;tooltip.replaceChildren();}
    };
    const showHover=(anchor,event)=>{
      const active=nodes[Number(anchor.querySelector('.dependency-node')?.dataset.index)];
      if(!active)return;
      clearHover();
      active.element.parentElement.classList.add('is-active');
      for(const edge of edges){
        if(edge.to===active.index){
          edge.element.classList.add('is-highlighted');
          edge.element.setAttribute('marker-end','url(#dep-arrow-highlight)');
          highlightedEdges.append(edge.element);
          nodes[edge.from]?.element.parentElement.classList.add('is-dependency');
        }
      }
      if(!tooltip)return;
      const addField=(label,value,math=false)=>{
        if(!value)return;
        const section=document.createElement('div');section.className='dependency-tooltip-field';
        const heading=document.createElement('strong');heading.textContent=label;section.append(heading);
        const content=document.createElement('div');content.textContent=math&&!/[\\$]/.test(value)?`$$${value}$$`:value;
        section.append(content);tooltip.append(section);
      };
      addField('@name',anchor.dataset.docName||active.element.querySelector('.dependency-name').textContent);
      addField('@NL',anchor.dataset.docNl);
      addField('@formula',anchor.dataset.docFormula,true);
      if(!tooltip.childElementCount){tooltip.hidden=true;return;}
      tooltip.hidden=false;
      if(window.renderMathInElement)renderMathInElement(tooltip,{delimiters:[{left:'$$',right:'$$',display:true},{left:'$',right:'$',display:false}],throwOnError:false});
      positionTooltip(event);
    };
    const positionTooltip=event=>{
      if(!tooltip||tooltip.hidden||!event)return;
      const gap=14,rect=tooltip.getBoundingClientRect();
      const left=Math.min(event.clientX+gap,window.innerWidth-rect.width-10);
      const top=Math.min(event.clientY+gap,window.innerHeight-rect.height-10);
      tooltip.style.left=`${Math.max(10,left)}px`;tooltip.style.top=`${Math.max(10,top)}px`;
    };
    dependencyGraph.addEventListener('pointerover',event=>{
      const anchor=event.target.closest('.dependency-link');
      if(anchor&&!selectedAnchor)showHover(anchor,event);
    });
    dependencyGraph.addEventListener('pointermove',event=>{if(!selectedAnchor&&!tooltip?.hidden)positionTooltip(event);});
    dependencyGraph.addEventListener('pointerout',event=>{
      const anchor=event.target.closest('.dependency-link');
      if(anchor&&!selectedAnchor&&!anchor.contains(event.relatedTarget))clearHover();
    });
    dependencyGraph.addEventListener('focusin',event=>{
      const anchor=event.target.closest('.dependency-link');if(anchor&&!selectedAnchor)showHover(anchor);
    });
    dependencyGraph.addEventListener('focusout',event=>{
      if(event.target.closest('.dependency-link')&&!selectedAnchor)clearHover();
    });
    dependencyGraph.addEventListener('contextmenu',event=>{
      event.preventDefault();
      selectedAnchor=null;
      clearHover();
    });

    // Tarjan SCCs condense accidental cycles into single DAG layers.
    let nextIndex=0;
    const indices=Array(count).fill(-1),low=Array(count).fill(0),stack=[],onStack=Array(count).fill(false),components=[];
    const visit=v=>{
      indices[v]=low[v]=nextIndex++;stack.push(v);onStack[v]=true;
      for(const w of outgoing[v]){
        if(indices[w]===-1){visit(w);low[v]=Math.min(low[v],low[w]);}
        else if(onStack[w])low[v]=Math.min(low[v],indices[w]);
      }
      if(low[v]===indices[v]){
        const component=[];let w;
        do{w=stack.pop();onStack[w]=false;component.push(w);}while(w!==v);
        components.push(component);
      }
    };
    for(let i=0;i<count;i++)if(indices[i]===-1)visit(i);
    const componentOf=Array(count);
    components.forEach((component,id)=>component.forEach(node=>componentOf[node]=id));
    const dag=components.map(()=>new Set()),degree=components.map(()=>0);
    for(const edge of edges){
      const a=componentOf[edge.from],b=componentOf[edge.to];
      if(a!==b&&!dag[a].has(b)){dag[a].add(b);degree[b]++;}
    }
    const queue=[];
    degree.forEach((n,i)=>{if(!n)queue.push(i);});
    const rank=Array(components.length).fill(0);
    for(let q=0;q<queue.length;q++){
      const c=queue[q];
      for(const d of dag[c]){rank[d]=Math.max(rank[d],rank[c]+1);if(--degree[d]===0)queue.push(d);}
    }
    nodes.forEach(node=>node.rank=rank[componentOf[node.index]]);
    const layers=[];
    for(const node of nodes)(layers[node.rank]||(layers[node.rank]=[])).push(node);
    for(const layer of layers)layer.sort((a,b)=>a.index-b.index);
    // Document pages carry the section titles assigned by the generator.
    const sections=new Map();
    for(const node of nodes){
      const anchor=node.element.parentElement;
      const key=anchor.dataset.sectionId||anchor.getAttribute('href').split('#')[0];
      if(!sections.has(key))sections.set(key,{title:anchor.dataset.section||key,members:[]});
      sections.get(key).members.push(node);
    }
    let layoutMode='layered';
    const svgNS='http://www.w3.org/2000/svg';
    const panelLayer=document.createElementNS(svgNS,'g');
    panelLayer.setAttribute('class','dependency-panel-layer');
    dependencyGraph.insertBefore(panelLayer,dependencyGraph.querySelector('.dependency-edges'));
    const orderNodes=()=>layers.forEach(layer=>layer.forEach((node,index)=>node.order=index));
    orderNodes();
    // Sugiyama-style alternating barycenter sweeps reduce edge crossings.
    for(let sweep=0;sweep<6;sweep++){
      const forward=sweep%2===0;
      const start=forward?1:layers.length-2,end=forward?layers.length:-1,step=forward?1:-1;
      for(let r=start;r!==end;r+=step){
        const adjacent=forward?r-1:r+1;
        const neighbors=forward?incoming:outgoing;
        layers[r].sort((a,b)=>{
          const bary=node=>{
            const peers=neighbors[node.index].filter(i=>nodes[i].rank===adjacent);
            return peers.length?peers.reduce((sum,i)=>sum+nodes[i].order,0)/peers.length:node.order;
          };
          return bary(a)-bary(b)||a.index-b.index;
        });
        orderNodes();
      }
    }
    let direction='lr',layoutWidth=0,layoutHeight=0;
    const layout=()=>{
      panelLayer.replaceChildren();
      dependencyGraph.dataset.layout=layoutMode;
      const margin=80;
      if(layoutMode==='sections'){
        const panels=[...sections.values()].map(section=>{
          const members=[...section.members].sort((a,b)=>a.rank-b.rank||a.index-b.index);
          const columns=Math.min(5,Math.max(1,Math.ceil(Math.sqrt(members.length*1.4))));
          const cellWidth=Math.max(...members.map(node=>node.width))+48;
          const rows=Math.ceil(members.length/columns);
          return {...section,members,columns,cellWidth,width:columns*cellWidth+48,height:rows*100+88};
        });
        const targetWidth=Math.max(...panels.map((panel,i)=>i%2===0?panel.width+(panels[i+1]?.width||0)+64:0));
        let x=margin,y=margin,rowHeight=0;
        panels.forEach((panel,index)=>{
          if(x>margin&&x+panel.width>margin+targetWidth){x=margin;y+=rowHeight+64;rowHeight=0;}
          const group=document.createElementNS(svgNS,'g');
          group.setAttribute('class','dependency-section-panel');
          group.style.setProperty('--panel-hue',String(index*137.508%360));
          const rect=document.createElementNS(svgNS,'rect');
          for(const [key,value] of Object.entries({x,y,width:panel.width,height:panel.height,rx:16}))rect.setAttribute(key,value);
          const heading=document.createElementNS(svgNS,'foreignObject');
          for(const [key,value] of Object.entries({x:x+24,y:y+12,width:panel.width-48,height:40}))heading.setAttribute(key,value);
          const label=document.createElement('div');label.className='dependency-section-title';
          label.textContent=panel.title;heading.append(label);
          if(window.renderMathInElement)renderMathInElement(label,{delimiters:[{left:'$',right:'$',display:false}],throwOnError:false});
          const title=document.createElementNS(svgNS,'title');title.textContent=panel.title;
          group.append(title,rect,heading);panelLayer.append(group);
          panel.members.forEach((node,i)=>{
            const col=direction==='lr'?i%panel.columns:Math.floor(i/Math.ceil(panel.members.length/panel.columns));
            const row=direction==='lr'?Math.floor(i/panel.columns):i%Math.ceil(panel.members.length/panel.columns);
            node.x=x+24+panel.cellWidth*(col+0.5);node.y=y+88+row*100;
            node.order=i;
          });
          x+=panel.width+64;rowHeight=Math.max(rowHeight,panel.height);
        });
        layoutWidth=Math.max(...panels.map((p,i)=>Number(panelLayer.children[i].querySelector('rect').getAttribute('x'))+p.width))+margin;
        layoutHeight=y+rowHeight+margin;
      }else{
        const layerGap=direction==='lr'?430:150,rowGap=direction==='lr'?116:360;
        const maxLayer=Math.max(0,layers.length-1),maxRows=Math.max(0,...layers.map(layer=>layer.length-1));
        layoutWidth=margin*2+320+(direction==='lr'?maxLayer*layerGap:maxRows*rowGap);
        layoutHeight=margin*2+80+(direction==='lr'?maxRows*rowGap:maxLayer*layerGap);
        for(let rank=0;rank<layers.length;rank++)layers[rank].forEach((node,i)=>{
          node.order=i;
          node.x=margin+160+(direction==='lr'?rank*layerGap:i*rowGap);
          node.y=margin+28+(direction==='lr'?i*rowGap:rank*layerGap);
        });
      }
      for(const node of nodes)node.element.setAttribute('transform',`translate(${node.x} ${node.y})`);
      updateEdges();
    };
    const updateEdges=()=>{
      for(const edge of edges){
        const from=nodes[edge.from],to=nodes[edge.to];
        let x1,y1,x2,y2,d;
        if(layoutMode==='sections'&&from!==to){
          // Panel grids contain edges in every direction; enter the facing node border.
          const dx=to.x-from.x,dy=to.y-from.y;
          if(Math.abs(dx)>Math.abs(dy)*1.3){
            const side=dx>=0?1:-1;
            x1=from.x+side*from.width/2;y1=from.y;
            x2=to.x-side*to.width/2;y2=to.y;
            const mid=(x1+x2)/2;
            d=`M ${x1} ${y1} C ${mid} ${y1}, ${mid} ${y2}, ${x2} ${y2}`;
          }else{
            const side=dy>=0?1:-1;
            x1=from.x;y1=from.y+side*from.height/2;
            x2=to.x;y2=to.y-side*to.height/2;
            const mid=(y1+y2)/2;
            d=`M ${x1} ${y1} C ${x1} ${mid}, ${x2} ${mid}, ${x2} ${y2}`;
          }
        }else if(from.rank===to.rank){
          const side=from.order<=to.order?1:-1;
          if(direction==='lr'){
            x1=from.x;y1=from.y+side*from.height/2;
            x2=to.x;y2=to.y-side*to.height/2;
            const bend=Math.max(50,Math.abs(y2-y1)*0.6);
            d=`M ${x1} ${y1} C ${x1-bend} ${y1}, ${x2-bend} ${y2}, ${x2} ${y2}`;
          }else{
            x1=from.x+side*from.width/2;y1=from.y;
            x2=to.x-side*to.width/2;y2=to.y;
            const bend=Math.max(50,Math.abs(x2-x1)*0.6);
            d=`M ${x1} ${y1} C ${x1} ${y1-bend}, ${x2} ${y2-bend}, ${x2} ${y2}`;
          }
        }else if(direction==='lr'){
          x1=from.x+from.width/2;y1=from.y;x2=to.x-to.width/2;y2=to.y;
          const mid=(x1+x2)/2;d=`M ${x1} ${y1} C ${mid} ${y1}, ${mid} ${y2}, ${x2} ${y2}`;
        }else{
          x1=from.x;y1=from.y+from.height/2;x2=to.x;y2=to.y-to.height/2;
          const mid=(y1+y2)/2;d=`M ${x1} ${y1} C ${x1} ${mid}, ${x2} ${mid}, ${x2} ${y2}`;
        }
        edge.element.setAttribute('d',d);
      }
    };
    layout();
    let view={x:0,y:0,width:layoutWidth,height:layoutHeight},viewCustomized=false;
    const setView=()=>{
      dependencyGraph.setAttribute('viewBox',`${view.x} ${view.y} ${view.width} ${view.height}`);
      // Arrowheads use screen pixels, matching the non-scaling edge strokes.
      const matrix=dependencyGraph.getScreenCTM();
      const scale=matrix?Math.hypot(matrix.a,matrix.b):1;
      if(scale>0)for(const [id,pixels] of [['dep-arrow',9],['dep-arrow-highlight',12]]){
        const marker=dependencyGraph.querySelector('#'+id);
        marker.setAttribute('markerWidth',pixels/scale);
        marker.setAttribute('markerHeight',pixels/scale);
      }
    };
    const fitGraph=()=>{
      if(!dependencyGraph.clientWidth||!dependencyGraph.clientHeight)return;
      const padding=60;
      const left=layoutMode==='sections'?20:Math.min(...nodes.map(node=>node.x-node.width/2))-padding;
      const right=layoutMode==='sections'?layoutWidth-20:Math.max(...nodes.map(node=>node.x+node.width/2))+padding;
      const top=layoutMode==='sections'?20:Math.min(...nodes.map(node=>node.y-node.height/2))-padding;
      const bottom=layoutMode==='sections'?layoutHeight-20:Math.max(...nodes.map(node=>node.y+node.height/2))+padding;
      const centerX=(left+right)/2,centerY=(top+bottom)/2;
      let width=right-left,height=bottom-top;
      const aspect=dependencyGraph.clientWidth/dependencyGraph.clientHeight;
      if(width/height<aspect)width=height*aspect;
      else height=width/aspect;
      view={x:centerX-width/2,y:centerY-height/2,width,height};viewCustomized=false;setView();
    };
    const zoomAt=(factor,x=view.x+view.width/2,y=view.y+view.height/2)=>{
      const width=Math.max(60,Math.min(Math.max(layoutWidth,layoutHeight)*4,view.width*factor));
      const height=width*view.height/view.width;
      const ratio=width/view.width;
      view={x:x-(x-view.x)*ratio,y:y-(y-view.y)*ratio,width,height};viewCustomized=true;setView();
    };
    fitGraph();
    if(window.ResizeObserver){
      new ResizeObserver(()=>{if(!viewCustomized)fitGraph();}).observe(dependencyGraph);
    }
    toolbar?.addEventListener('click',event=>{
      const action=event.target.closest('button')?.dataset.graphAction;
      if(action==='zoom-in')zoomAt(0.8);
      if(action==='zoom-out')zoomAt(1.25);
      if(action==='fit')fitGraph();
      if(action==='reset'){layout();fitGraph();}
      if(action==='direction'){
        direction=direction==='lr'?'tb':'lr';
        event.target.closest('button').textContent=direction==='lr'?'Top to bottom':'Left to right';
        layout();fitGraph();
      }
    });
    toolbar?.querySelector('[data-graph-action=layout]')?.addEventListener('change',event=>{
      layoutMode=event.target.value==='sections'?'sections':'layered';
      layout();fitGraph();
    });
    dependencyGraph.addEventListener('wheel',event=>{
      event.preventDefault();
      const point=dependencyGraph.createSVGPoint();point.x=event.clientX;point.y=event.clientY;
      const graphPoint=point.matrixTransform(dependencyGraph.getScreenCTM().inverse());
      zoomAt(event.deltaY<0?0.9:1.1,graphPoint.x,graphPoint.y);
    },{passive:false});
    const graphPoint=(clientX,clientY,matrix)=>{
      const point=dependencyGraph.createSVGPoint();point.x=clientX;point.y=clientY;
      return point.matrixTransform(matrix);
    };
    let drag=null,suppressPointerClick=false;
    dependencyGraph.addEventListener('dragstart',event=>event.preventDefault());
    dependencyGraph.addEventListener('pointerdown',event=>{
      if(event.button!==0)return;
      const matrix=dependencyGraph.getScreenCTM()?.inverse();
      if(!matrix)return;
      const nodeElement=event.target.closest('.dependency-node');
      const node=nodeElement?nodes[Number(nodeElement.dataset.index)]:null;
      drag={pointerId:event.pointerId,node,anchor:nodeElement?.parentElement,
        matrix,start:graphPoint(event.clientX,event.clientY,matrix),
        clientX:event.clientX,clientY:event.clientY,nodeX:node?.x,nodeY:node?.y,
        viewX:view.x,viewY:view.y,moved:false};
      suppressPointerClick=false;
      // Capture only once a drag starts, so ordinary clicks retain their link target.
    });
    dependencyGraph.addEventListener('pointermove',event=>{
      if(!drag||event.pointerId!==drag.pointerId)return;
      if(Math.hypot(event.clientX-drag.clientX,event.clientY-drag.clientY)>4)drag.moved=true;
      if(!drag.moved)return;
      if(!dependencyGraph.hasPointerCapture(event.pointerId))dependencyGraph.setPointerCapture(event.pointerId);
      viewCustomized=true;
      const point=graphPoint(event.clientX,event.clientY,drag.matrix);
      const dx=point.x-drag.start.x,dy=point.y-drag.start.y;
      if(drag.node){
        drag.anchor.dataset.dragging='1';
        drag.node.x=drag.nodeX+dx;drag.node.y=drag.nodeY+dy;
        drag.node.element.setAttribute('transform',`translate(${drag.node.x} ${drag.node.y})`);
        updateEdges();
      }else{
        dependencyGraph.dataset.panning='1';
        view.x=drag.viewX-dx;view.y=drag.viewY-dy;setView();
      }
    });
    const endDrag=(event,cancelled)=>{
      if(!drag||event.pointerId!==drag.pointerId)return;
      const {anchor,moved}=drag;
      if(anchor)delete anchor.dataset.dragging;
      delete dependencyGraph.dataset.panning;
      suppressPointerClick=moved&&!cancelled;
      drag=null;
      if(dependencyGraph.hasPointerCapture(event.pointerId))dependencyGraph.releasePointerCapture(event.pointerId);
      if(anchor&&!moved&&!cancelled){selectedAnchor=anchor;showHover(anchor,event);}
    };
    dependencyGraph.addEventListener('pointerup',event=>endDrag(event,false));
    dependencyGraph.addEventListener('pointercancel',event=>endDrag(event,true));
    dependencyGraph.addEventListener('click',event=>{
      if(event.detail>0&&suppressPointerClick){event.preventDefault();event.stopPropagation();suppressPointerClick=false;return;}
      if(event.detail===0)return; // Keep keyboard activation of declaration links.
      const anchor=event.target.closest('.dependency-link');
      if(anchor){
        event.preventDefault();
        selectedAnchor=anchor;
        showHover(anchor,event);
      }
    },true);
    dependencyGraph.addEventListener('dblclick',event=>{
      const anchor=event.target.closest('.dependency-link');
      if(anchor){event.preventDefault();window.location.assign(anchor.getAttribute('href'));}
    },true);
    document.addEventListener('keydown',event=>{
      if(event.key==='Escape'){selectedAnchor=null;clearHover();}
    });
    for(const node of nodes)node.element.parentElement.setAttribute('aria-label',`Select ${node.element.querySelector('.dependency-name').textContent}; right-click or press Escape to clear selection`);
  }
  if(document.body.dataset.live==='1'){
    let prior=null;
    setInterval(async()=>{try{const now=await (await fetch('/__myne_epoch',{cache:'no-store'})).text();if(prior!==null&&now!==prior)location.reload();prior=now}catch(_){}},1000);
  }
});
