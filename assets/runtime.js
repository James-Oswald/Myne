document.addEventListener('DOMContentLoaded',()=>{
  if(window.renderMathInElement)renderMathInElement(document.body,{delimiters:[{left:'$$',right:'$$',display:true},{left:'$',right:'$',display:false}],throwOnError:false});
  const themeToggle=document.querySelector('.theme-toggle');
  if(themeToggle){
    const applyTheme=theme=>{
      document.documentElement.dataset.theme=theme;
      const dark=theme==='dark';
      themeToggle.setAttribute('aria-pressed',String(dark));
      themeToggle.textContent=dark?'Light mode':'Dark mode';
    };
    applyTheme(document.documentElement.dataset.theme||'light');
    themeToggle.addEventListener('click',()=>{
      const theme=document.documentElement.dataset.theme==='dark'?'light':'dark';
      applyTheme(theme);
      try{localStorage.setItem('myne-theme',theme)}catch(_){}
    });
  }
  if(document.body.dataset.live==='1'){
    let prior=null;
    setInterval(async()=>{try{const now=await (await fetch('/__myne_epoch',{cache:'no-store'})).text();if(prior!==null&&now!==prior)location.reload();prior=now}catch(_){}},1000);
  }
});
