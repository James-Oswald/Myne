document.addEventListener('DOMContentLoaded',()=>{
  if(window.renderMathInElement)renderMathInElement(document.body,{delimiters:[{left:'$$',right:'$$',display:true},{left:'$',right:'$',display:false}],throwOnError:false});
  if(document.body.dataset.live==='1'){
    let prior=null;
    setInterval(async()=>{try{const now=await (await fetch('/__myne_epoch',{cache:'no-store'})).text();if(prior!==null&&now!==prior)location.reload();prior=now}catch(_){}},1000);
  }
});
