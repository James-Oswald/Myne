document.addEventListener('DOMContentLoaded',()=>{
  if(window.renderMathInElement)renderMathInElement(document.body,{delimiters:[{left:'$$',right:'$$',display:true},{left:'$',right:'$',display:false}],throwOnError:false});
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
    const tooltip=document.querySelector('.dependency-tooltip');
    const clearHover=()=>{
      for(const node of nodes)node.element.parentElement.classList.remove('is-active','is-dependency');
      for(const edge of edges)edge.element.classList.remove('is-highlighted');
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
      if(anchor)showHover(anchor,event);
    });
    dependencyGraph.addEventListener('pointermove',event=>{if(!tooltip?.hidden)positionTooltip(event);});
    dependencyGraph.addEventListener('pointerout',event=>{
      const anchor=event.target.closest('.dependency-link');
      if(anchor&&!anchor.contains(event.relatedTarget))clearHover();
    });
    dependencyGraph.addEventListener('focusin',event=>{
      const anchor=event.target.closest('.dependency-link');if(anchor)showHover(anchor);
    });
    dependencyGraph.addEventListener('focusout',event=>{
      if(event.target.closest('.dependency-link'))clearHover();
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
      const layerGap=direction==='lr'?430:150,rowGap=direction==='lr'?116:360,margin=80;
      const maxLayer=Math.max(0,layers.length-1),maxRows=Math.max(0,...layers.map(layer=>layer.length-1));
      layoutWidth=margin*2+320+(direction==='lr'?maxLayer*layerGap:maxRows*rowGap);
      layoutHeight=margin*2+80+(direction==='lr'?maxRows*rowGap:maxLayer*layerGap);
      for(let r=0;r<layers.length;r++)layers[r].forEach((node,i)=>{
        node.order=i;
        if(direction==='lr'){node.x=margin+160+r*layerGap;node.y=margin+28+i*rowGap;}
        else{node.x=margin+160+i*rowGap;node.y=margin+28+r*layerGap;}
        node.element.setAttribute('transform',`translate(${node.x} ${node.y})`);
      });
      updateEdges();
    };
    const updateEdges=()=>{
      for(const edge of edges){
        const from=nodes[edge.from],to=nodes[edge.to];
        let x1,y1,x2,y2,d;
        if(from.rank===to.rank){
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
    const setView=()=>dependencyGraph.setAttribute('viewBox',`${view.x} ${view.y} ${view.width} ${view.height}`);
    const fitGraph=()=>{
      if(!dependencyGraph.clientWidth||!dependencyGraph.clientHeight)return;
      const padding=60;
      const left=Math.min(...nodes.map(node=>node.x-node.width/2))-padding;
      const right=Math.max(...nodes.map(node=>node.x+node.width/2))+padding;
      const top=Math.min(...nodes.map(node=>node.y-node.height/2))-padding;
      const bottom=Math.max(...nodes.map(node=>node.y+node.height/2))+padding;
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
      dependencyGraph.setPointerCapture(event.pointerId);
      event.preventDefault();
    });
    dependencyGraph.addEventListener('pointermove',event=>{
      if(!drag||event.pointerId!==drag.pointerId)return;
      if(Math.hypot(event.clientX-drag.clientX,event.clientY-drag.clientY)>4)drag.moved=true;
      if(!drag.moved)return;
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
      suppressPointerClick=true;
      drag=null;
      if(dependencyGraph.hasPointerCapture(event.pointerId))dependencyGraph.releasePointerCapture(event.pointerId);
      if(anchor&&!moved&&!cancelled)window.location.assign(anchor.getAttribute('href'));
    };
    dependencyGraph.addEventListener('pointerup',event=>endDrag(event,false));
    dependencyGraph.addEventListener('pointercancel',event=>endDrag(event,true));
    dependencyGraph.addEventListener('click',event=>{
      if(event.detail>0&&suppressPointerClick){event.preventDefault();event.stopPropagation();suppressPointerClick=false;}
    },true);
    for(const node of nodes)node.element.parentElement.setAttribute('aria-label',`Open ${node.element.querySelector('.dependency-name').textContent}`);
  }
  if(document.body.dataset.live==='1'){
    let prior=null;
    setInterval(async()=>{try{const now=await (await fetch('/__myne_epoch',{cache:'no-store'})).text();if(prior!==null&&now!==prior)location.reload();prior=now}catch(_){}},1000);
  }
});
